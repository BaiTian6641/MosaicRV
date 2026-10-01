// ============================================================================
// tb_lease.cpp -- CASE=lease.conflict_and_cancel, work package I-024.
//
// The DUT is never its own oracle. Every output and every piece of internal
// state is compared against an independent C++ shadow written from the
// *contract* documented in rtl/core/mosaic_lease_alloc.sv -- not from the RTL's
// structure. The shadow keeps its own lease ledger (live / ever-used / how the
// current epoch ended / generation / the mask and slot of every reservation) and
// *separately* a per-pool occupancy bitmap and per-pool count, so comparing
// those two structures against the DUT's `o_occ`/`o_occ_count` checks a
// derivation rather than restating it. Its arbitration walks a rotated candidate
// *list* where the DUT walks a modular index, and its terminal classification is
// written as named rules rather than as the RTL's if-chain, so a defect in one
// cannot cancel against the same defect in the other.
//
// Geometry is read from the elaborated DUT (`o_req_count`, `o_pools`,
// `o_size_*`, `o_leases`, `o_gen_w`, `o_slot_w`, `o_lease_w`), so this file
// contains no pool size, no lease count and no generation width: the shadow
// sizes itself from the hardware, and the checks after reset assert the
// relations the rest of the file depends on.
//
// Two-snapshot rule, applied everywhere below: values read *before* the clock
// edge describe the cycle under test and are what the DUT *presents*
// (`req_ready`, `grant_id`, `grant_slot`, the terminal reports); values read
// *after* the edge are registered state and are what is true *now* (occupancy,
// the ledger, the counters). They are never mixed in one comparison.
//
// Phases, each of which resets first and can fail on its own:
//
//   1. reset-state     the documented cold ledger, and a reset taken while
//                      leases are live and every pool occupied.
//   2. conflict        two requesters need the one shared MUL/DIV slot in the
//                      same cycle: exactly one wins, the loser has no side
//                      effect, and it is granted on the edge the winner's
//                      release returns the slot.
//   3. partial         a request naming {FU, credit} with only the FU free:
//                      nothing is granted and the FU slot is *not* consumed.
//                      This is the card's "reserve the FU and wait forever for
//                      WB" failure, and the phase shows it is unreachable.
//   4. cancel          a flush mid-operation returns everything exactly once; a
//                      later request succeeds with a new lease id; the old id is
//                      rejected as stale.
//   5. terminal-errors double release, release of a never-granted id, an
//                      out-of-range id, a never-issued generation on a *live*
//                      lease, cancel-after-release and release-after-cancel:
//                      each is counted, and none moves a resource.
//   6. same-cycle      a credit returned on an edge funds a grant on that same
//                      edge, and the slot that comes back is the slot reused.
//   7. refusal         replay: the same stimulus with and without a refused
//                      request produces *identical* state, cycle by cycle.
//   8. wrap            the generation counted through its whole modulus, so the
//                      wrap is exercised rather than assumed.
//   9. random          a soak of random request sets, random terminal timing and
//                      random credit returns, shadow-compared every cycle, with
//                      the conservation identity re-checked every cycle.
//
// Standing invariants, checked every cycle of every phase rather than in one
// place:
//
//   * `o_grant_count == o_rel_ok_count + o_can_ok_count + o_live_count`.
//   * `o_live_count` equals the population of `o_lease_live`.
//   * each `o_occ_count[p]` equals the population of that pool's real slots in
//     `o_occ`, and `o_occ` equals the union of the live leases' reservations --
//     derived inside the shadow from its ledger, which is why a partial
//     reservation cannot hide.
//   * no pool slot is occupied without a live lease holding it, which is the
//     structural form of "all or none".
// ============================================================================

#include <verilated.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "sim_common.h"
#include "Vmosaic_lease_alloc_tb.h"

namespace {

// Thrown on the first failed check, so the report names one defect rather than a
// thousand consequences of it.
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

// Resource-class bit positions in a request mask, and the pool order in `o_occ`.
// The pool order is the module's layout, documented in its port list; naming it
// once here means a phase that means "the ALU class" cannot silently name the
// wrong bit.
enum { POOL_ALU = 0, POOL_MD = 1, POOL_WB = 2, POOL_NET = 3 };
enum {
  MASK_ALU = 1u << POOL_ALU,
  MASK_MD = 1u << POOL_MD,
  MASK_WB = 1u << POOL_WB,
  MASK_NET = 1u << POOL_NET
};

const char* PoolName(uint32_t pool) {
  switch (pool) {
    case POOL_ALU: return "alu";
    case POOL_MD: return "md";
    case POOL_WB: return "wb";
    case POOL_NET: return "net";
    default: return "?";
  }
}

// Extract a bit field from a scalar (<= 32 bit) DUT port value.
uint32_t Field(uint32_t value, uint32_t lsb, uint32_t width) {
  const uint32_t mask = (width >= 32) ? 0xffffffffu : ((1u << width) - 1u);
  return (value >> lsb) & mask;
}

uint32_t GenMask(uint32_t gen_w) {
  return (gen_w >= 32) ? 0xffffffffu : ((1u << gen_w) - 1u);
}

// --------------------------------------------------------------- identities
// A lease id is a record index *and* a generation. An index alone is not an
// identity -- that is the whole reason the generation exists -- so the shadow
// and the stimulus both carry the pair.
struct LeaseId {
  uint32_t row = 0;
  uint32_t gen = 0;

  bool operator==(const LeaseId& other) const { return row == other.row && gen == other.gen; }
  bool operator!=(const LeaseId& other) const { return !(*this == other); }
  std::string str() const { return "(" + Dec(row) + ",g" + Dec(gen) + ")"; }
};

// ---------------------------------------------------------------- geometry
struct Geom {
  uint32_t req_count = 0;
  uint32_t pools = 0;
  uint32_t leases = 0;
  uint32_t max_sz = 0;
  uint32_t gen_w = 0;
  uint32_t slot_w = 0;
  uint32_t lease_w = 0;
  uint32_t row_w = 0;
  uint32_t idx_w = 0;
  uint32_t size[4] = {0, 0, 0, 0};
};

// ---------------------------------------------------------------- stimulus
struct Stim {
  std::vector<bool> req_valid;
  std::vector<uint32_t> req_mask;
  bool rel_valid = false;
  LeaseId rel;
  bool can_valid = false;
  LeaseId can;

  std::string str() const {
    std::string text = "[req";
    for (size_t i = 0; i < req_valid.size(); i++) {
      text += " " + Dec(i) + ":";
      text += req_valid[i] ? "v" : "-";
      text += "=0x" + mosaic::Hex(req_mask[i], 1);
    }
    text += " rel=";
    text += rel_valid ? rel.str() : std::string("-");
    text += " can=";
    text += can_valid ? can.str() : std::string("-");
    text += "]";
    return text;
  }
};

Stim MakeStim(const Geom& g) {
  Stim s;
  s.req_valid.assign(g.req_count, false);
  s.req_mask.assign(g.req_count, 0);
  return s;
}

void AddReq(Stim* s, uint32_t port, uint32_t mask) {
  (*s).req_valid[port] = true;
  (*s).req_mask[port] = mask;
}

// ============================================================================
// The shadow
//
// An independent model of the documented contract. Every rule below is a
// sentence from the module's header, not a step of the RTL.
// ============================================================================
class ShadowLease {
 public:
  explicit ShadowLease(const Geom& geom) : g_(geom) {
    gen_mask_ = GenMask(geom.gen_w);
    // The generation array is *not* part of the reset contract (the module does
    // not clear it, by the project's rule that reset cost is control state and
    // not storage), so it is left alone here too and only compared for records
    // whose `used` bit says it has meaning.
    gen_.assign(geom.leases, 0);
    mask_.assign(geom.leases * geom.pools, 0);
    slot_.assign(geom.leases * geom.pools, 0);
    Reset();
  }

  void Reset() {
    live_.assign(g_.leases, false);
    used_.assign(g_.leases, false);
    cancelled_.assign(g_.leases, false);
    occ_.assign(g_.pools, std::vector<uint8_t>(g_.max_sz, 0));
    occ_count_.assign(g_.pools, 0);
    grant_count_ = 0;
    rel_ok_ = 0;
    can_ok_ = 0;
    rel_stale_ = 0;
    can_stale_ = 0;
    rel_repeat_ = 0;
    can_repeat_ = 0;
    rel_after_can_ = 0;
    can_after_rel_ = 0;
    live_count_ = 0;
    rr_ = 0;
  }

  // ------------------------------------------------------ what the DUT shows
  struct Out {
    std::vector<uint8_t> ready;
    std::vector<LeaseId> id;
    std::vector<uint32_t> slot;  // per requester, per pool
    uint8_t rel_ok = 0, rel_stale = 0, rel_repeat = 0, rel_after_cancel = 0;
    uint8_t can_ok = 0, can_stale = 0, can_repeat = 0, can_after_release = 0;
    uint32_t rel_row = 0, can_row = 0;
    uint32_t rr_next = 0;
    uint32_t grants = 0;
  };

  // Pure: reads the pre-edge ledger and the cycle's stimulus and mutates
  // nothing. The terminals are classified against the pre-edge epoch, and the
  // release is applied before the cancel is classified, so a release and a
  // cancel for one lease in one cycle is the documented "release wins, the
  // cancel is reported as `can_after_release`" rather than a don't-care.
  Out Eval(const Stim& s) const {
    Out o;
    o.ready.assign(g_.req_count, 0);
    o.id.assign(g_.req_count, LeaseId{});
    o.slot.assign(g_.req_count * g_.pools, 0);
    o.rr_next = rr_;

    std::vector<bool> live_now = live_;
    std::vector<std::vector<uint8_t>> taken = occ_;

    // ---- release
    if (s.rel_valid) {
      const LeaseId& id = s.rel;
      if (id.row >= g_.leases) {
        o.rel_stale = 1;
      } else if (!used_[id.row]) {
        o.rel_stale = 1;  // never granted since reset
      } else if ((id.gen & gen_mask_) != gen_[id.row]) {
        o.rel_stale = 1;  // a generation never issued, or a superseded epoch
      } else if (!live_now[id.row]) {
        if (cancelled_[id.row]) o.rel_after_cancel = 1;
        else o.rel_repeat = 1;
      } else {
        o.rel_ok = 1;
        o.rel_row = id.row;
        live_now[id.row] = false;
        ReleaseSlots(&taken, id.row);
      }
    }

    // ---- cancel, against the state the release leaves behind
    if (s.can_valid) {
      const LeaseId& id = s.can;
      if (id.row >= g_.leases) {
        o.can_stale = 1;
      } else if (!used_[id.row]) {
        o.can_stale = 1;
      } else if ((id.gen & gen_mask_) != gen_[id.row]) {
        o.can_stale = 1;
      } else if (!live_now[id.row]) {
        if (cancelled_[id.row]) o.can_repeat = 1;
        else o.can_after_release = 1;
      } else {
        o.can_ok = 1;
        o.can_row = id.row;
        live_now[id.row] = false;
        ReleaseSlots(&taken, id.row);
      }
    }

    // ---- round robin. The candidates are walked as a rotated *list*, which is
    // a different construction from the DUT's modular index arithmetic.
    for (uint32_t k = 0; k < g_.req_count; k++) {
      const uint32_t cand = (rr_ + k) % g_.req_count;
      if (!s.req_valid[cand]) continue;

      // The lowest free slot of each class the request names.
      std::vector<uint32_t> chosen(g_.pools, 0);
      bool satisfied = true;
      for (uint32_t p = 0; p < g_.pools; p++) {
        if (Field(s.req_mask[cand], p, 1) == 0) continue;
        bool found = false;
        for (uint32_t sl = 0; sl < g_.size[p]; sl++) {
          if (taken[p][sl] == 0) {
            chosen[p] = sl;
            found = true;
            break;
          }
        }
        if (!found) satisfied = false;
      }

      // The lowest record with no live lease. A record terminated by this
      // cycle's terminal is free from this edge, which is the documented
      // "returned before taken" order.
      uint32_t row = 0;
      bool row_found = false;
      for (uint32_t r = 0; r < g_.leases; r++) {
        if (!live_now[r]) {
          row = r;
          row_found = true;
          break;
        }
      }

      if (!satisfied || !row_found) continue;

      o.ready[cand] = 1;
      o.grants++;
      // A record never granted since reset starts at generation 0; otherwise the
      // current epoch steps by one.
      const uint32_t next_gen = used_[row] ? ((gen_[row] + 1) & gen_mask_) : 0;
      o.id[cand] = LeaseId{row, next_gen};
      for (uint32_t p = 0; p < g_.pools; p++) {
        if (Field(s.req_mask[cand], p, 1) == 0) continue;
        o.slot[cand * g_.pools + p] = chosen[p];
        taken[p][chosen[p]] = 1;
      }
      live_now[row] = true;
      o.rr_next = (cand + 1 == g_.req_count) ? 0 : cand + 1;
    }
    return o;
  }

  // Advance the ledger over one edge, in the documented order.
  void Apply(const Stim& s) {
    const Out o = Eval(s);

    // 1. The terminals kill their epochs and return what they held, before any
    //    grant is taken, so a credit returned on this edge can fund a grant on
    //    this edge.
    if (o.rel_ok) EndEpoch(o.rel_row, false);
    if (o.can_ok) EndEpoch(o.can_row, true);

    // 2. The grants. A grant may reuse a record terminated above, and then the
    //    grant's values win -- the same "later write wins" order the RTL has.
    for (uint32_t i = 0; i < g_.req_count; i++) {
      if (o.ready[i] == 0) continue;
      const uint32_t row = o.id[i].row;
      live_[row] = true;
      used_[row] = true;
      gen_[row] = o.id[i].gen;
      for (uint32_t p = 0; p < g_.pools; p++) {
        const uint32_t need = Field(s.req_mask[i], p, 1);
        mask_[row * g_.pools + p] = need;
        slot_[row * g_.pools + p] = o.slot[i * g_.pools + p];
        if (need) {
          occ_[p][o.slot[i * g_.pools + p]] = 1;
          occ_count_[p]++;
        }
      }
    }

    // 3. The counters and the pointer.
    grant_count_ += o.grants;
    rel_ok_ += o.rel_ok;
    can_ok_ += o.can_ok;
    rel_stale_ += o.rel_stale;
    can_stale_ += o.can_stale;
    rel_repeat_ += o.rel_repeat;
    can_repeat_ += o.can_repeat;
    rel_after_can_ += o.rel_after_cancel;
    can_after_rel_ += o.can_after_release;
    live_count_ = live_count_ + o.grants - o.rel_ok - o.can_ok;
    rr_ = o.rr_next;

    // 4. The shadow checks itself. These are model bugs, not DUT bugs, and are
    //    reported as such: a shadow that has drifted from its own contract
    //    cannot be used to judge the hardware.
    SelfCheck();
  }

  // ------------------------------------------------------------ observation
  const Geom& geom() const { return g_; }
  const std::vector<bool>& live() const { return live_; }
  const std::vector<bool>& used() const { return used_; }
  uint32_t gen(uint32_t row) const { return gen_[row]; }
  uint32_t mask_of(uint32_t row) const { return MaskOf(row); }
  uint32_t slots_of(uint32_t row, uint32_t pool) const { return slot_[row * g_.pools + pool]; }
  uint32_t rr() const { return rr_; }
  uint32_t occ_bit(uint32_t pool, uint32_t slot) const { return occ_[pool][slot]; }
  uint32_t occ_count(uint32_t pool) const { return occ_count_[pool]; }
  bool epoch_cancelled(uint32_t row) const { return cancelled_[row]; }
  uint64_t grant_count() const { return grant_count_; }
  uint64_t rel_ok() const { return rel_ok_; }
  uint64_t can_ok() const { return can_ok_; }
  uint64_t rel_stale() const { return rel_stale_; }
  uint64_t can_stale() const { return can_stale_; }
  uint64_t rel_repeat() const { return rel_repeat_; }
  uint64_t can_repeat() const { return can_repeat_; }
  uint64_t rel_after_can() const { return rel_after_can_; }
  uint64_t can_after_rel() const { return can_after_rel_; }
  uint64_t live_count() const { return live_count_; }
  uint32_t gen_mask() const { return gen_mask_; }

  uint32_t live_population() const {
    uint32_t n = 0;
    for (uint32_t r = 0; r < g_.leases; r++) {
      if (live_[r]) n++;
    }
    return n;
  }

  // Rows that are live right now, for aiming a terminal at a real lease.
  std::vector<uint32_t> live_rows() const {
    std::vector<uint32_t> rows;
    for (uint32_t r = 0; r < g_.leases; r++) {
      if (live_[r]) rows.push_back(r);
    }
    return rows;
  }

  std::vector<uint32_t> dead_used_rows() const {
    std::vector<uint32_t> rows;
    for (uint32_t r = 0; r < g_.leases; r++) {
      if (used_[r] && !live_[r]) rows.push_back(r);
    }
    return rows;
  }

 private:
  void EndEpoch(uint32_t row, bool cancelled) {
    ReleaseSlotsFromOcc(row);
    live_[row] = false;
    cancelled_[row] = cancelled;
  }

  void ReleaseSlotsFromOcc(uint32_t row) {
    for (uint32_t p = 0; p < g_.pools; p++) {
      if (MaskOf(row) & (1u << p)) {
        occ_[p][slot_[row * g_.pools + p]] = 0;
        occ_count_[p]--;
      }
    }
  }

  void ReleaseSlots(std::vector<std::vector<uint8_t>>* taken, uint32_t row) const {
    for (uint32_t p = 0; p < g_.pools; p++) {
      if (MaskOf(row) & (1u << p)) {
        (*taken)[p][slot_[row * g_.pools + p]] = 0;
      }
    }
  }

  uint32_t MaskOf(uint32_t row) const {
    uint32_t m = 0;
    for (uint32_t p = 0; p < g_.pools; p++) {
      if (mask_[row * g_.pools + p]) m |= (1u << p);
    }
    return m;
  }

  void SelfCheck() const {
    for (uint32_t p = 0; p < g_.pools; p++) {
      std::vector<uint8_t> derived(g_.max_sz, 0);
      for (uint32_t r = 0; r < g_.leases; r++) {
        if (live_[r] && (MaskOf(r) & (1u << p))) {
          derived[slot_[r * g_.pools + p]] = 1;
        }
      }
      if (derived != occ_[p]) {
        Fail("shadow",
             "the per-pool bitmap and the ledger disagree for pool " + std::string(PoolName(p)) +
                 ": the shadow's two structures must agree before it judges the DUT");
      }
      uint32_t now = 0;
      for (uint32_t sl = 0; sl < g_.max_sz; sl++) {
        now += derived[sl];
      }
      if (now != occ_count_[p]) {
        Fail("shadow", "the per-pool count and the ledger disagree for pool " +
                           std::string(PoolName(p)));
      }
    }
    if (live_count_ != live_population()) {
      Fail("shadow", "the live count and the live vector disagree");
    }
    if (grant_count_ != rel_ok_ + can_ok_ + live_count_) {
      Fail("shadow", "grant/release/cancel/live conservation is broken in the shadow");
    }
  }

  Geom g_;
  uint32_t gen_mask_ = 0;

  std::vector<bool> live_;
  std::vector<bool> used_;
  std::vector<bool> cancelled_;
  std::vector<uint32_t> gen_;
  std::vector<uint32_t> mask_;  // per record, per pool: 0/1
  std::vector<uint32_t> slot_;  // per record, per pool
  std::vector<std::vector<uint8_t>> occ_;
  std::vector<uint32_t> occ_count_;

  uint64_t grant_count_ = 0, rel_ok_ = 0, can_ok_ = 0;
  uint64_t rel_stale_ = 0, can_stale_ = 0, rel_repeat_ = 0, can_repeat_ = 0;
  uint64_t rel_after_can_ = 0, can_after_rel_ = 0, live_count_ = 0;
  uint32_t rr_ = 0;
};

// ============================================================================
// The harness
// ============================================================================
class Harness {
 public:
  Harness(Vmosaic_lease_alloc_tb* dut, mosaic::ClockDriver* clk, uint64_t max_cycles,
          const Geom& g)
      : dut_(dut), clk_(clk), max_cycles_(max_cycles), g_(g) {}

  void Phase(const std::string& name) { phase_ = name; }
  void Bind(ShadowLease* shadow) { shadow_ = shadow; }
  ShadowLease* shadow() const { return shadow_; }

  void Reset(int cycles) {
    for (int i = 0; i < cycles; i++) {
      Cycle(MakeStim(g_), /*rst=*/true);
    }
    if (shadow_ != nullptr) shadow_->Reset();
  }

  // One clock period, in the documented three steps. Returns the *pre-edge*
  // combinational answer by value; a reference would alias, and a phase that
  // held it across another Cycle would silently read the next cycle's outputs.
  ShadowLease::Out Cycle(const Stim& s, bool rst = false) {
    if (cycles_ >= max_cycles_) {
      Fail(phase_, "max-cycles (" + Dec(max_cycles_) + ") exhausted before the campaign finished");
    }
    Drive(s, rst);
    dut_->eval();

    ShadowLease::Out expected;
    const std::string where = phase_ + ": cycle " + Dec(clk_->cycle());
    if (!rst) {
      expected = shadow_->Eval(s);
      CompareComb(expected, s, where);
      shadow_->Apply(s);
    }

    dut_->clk = 1;
    dut_->eval();
    clk_->Tick();
    dut_->clk = 0;
    dut_->eval();
    ++cycles_;

    if (!rst) {
      CompareState(where);
      CheckStandingInvariants(where);
      ++comparisons_;
    }
    return expected;
  }

  uint64_t cycles() const { return cycles_; }
  uint64_t comparisons() const { return comparisons_; }

  // ------------------------------------------------------- DUT observation
  uint32_t occ_count(uint32_t pool) const {
    switch (pool) {
      case POOL_ALU: return dut_->o_occ_count_alu;
      case POOL_MD: return dut_->o_occ_count_md;
      case POOL_WB: return dut_->o_occ_count_wb;
      case POOL_NET: return dut_->o_occ_count_net;
      default: Fail(phase_, "no such pool"); return 0;
    }
  }

  uint32_t occ_bit(uint32_t pool, uint32_t slot) const {
    return Field(dut_->o_occ, pool * g_.max_sz + slot, 1);
  }

  uint32_t lease_slot(uint32_t row, uint32_t pool) const {
    return Field(dut_->o_lease_slot, (row * g_.pools + pool) * g_.slot_w, g_.slot_w);
  }

  uint32_t lease_gen(uint32_t row) const {
    return Field(dut_->o_lease_gen, row * g_.gen_w, g_.gen_w);
  }

  uint32_t rr() const { return dut_->o_rr; }

  // The whole observable state, for "nothing changed" assertions and for the
  // replay experiment. Registered observables only: this is what is true now.
  struct Snap {
    uint32_t occ = 0;
    uint32_t occ_count[4] = {0, 0, 0, 0};
    uint32_t live = 0, used = 0, end_kind = 0, rr = 0;
    uint32_t gen = 0, mask = 0, slot = 0;
    uint32_t counters[10] = {0};

    bool operator==(const Snap& o) const {
      if (occ != o.occ || live != o.live || used != o.used || end_kind != o.end_kind) return false;
      if (rr != o.rr || gen != o.gen || mask != o.mask || slot != o.slot) return false;
      for (int i = 0; i < 4; i++) {
        if (occ_count[i] != o.occ_count[i]) return false;
      }
      for (int i = 0; i < 10; i++) {
        if (counters[i] != o.counters[i]) return false;
      }
      return true;
    }
    bool operator!=(const Snap& o) const { return !(*this == o); }
    // The ledger part: what the unit *holds*. The rejection counters are reports
    // about a terminal event, and a rejected event is expected to move the one it
    // belongs to -- that is the whole point of counting it -- so they are
    // excluded here. They are still compared against the shadow every cycle.
    bool SameResources(const Snap& o) const {
      if (occ != o.occ || live != o.live || used != o.used || end_kind != o.end_kind) return false;
      if (rr != o.rr || gen != o.gen || mask != o.mask || slot != o.slot) return false;
      for (int i = 0; i < 4; i++) {
        if (occ_count[i] != o.occ_count[i]) return false;
      }
      // grant, release, cancel and live: the four counters the conservation
      // identity relates.
      return counters[0] == o.counters[0] && counters[1] == o.counters[1] &&
             counters[2] == o.counters[2] && counters[9] == o.counters[9];
    }
    std::string str() const {
      std::string text = "occ=0x" + mosaic::Hex(occ, 4) + " occ_count={";
      for (int i = 0; i < 4; i++) text += Dec(occ_count[i]) + (i == 3 ? "}" : ",");
      text += " live=0x" + mosaic::Hex(live, 1) + " used=0x" + mosaic::Hex(used, 1) +
              " end=0x" + mosaic::Hex(end_kind, 1) + " rr=" + Dec(rr) + " gen=0x" +
              mosaic::Hex(gen, 8) + " mask=0x" + mosaic::Hex(mask, 4) + " slot=0x" +
              mosaic::Hex(slot, 8) + " counters={";
      for (int i = 0; i < 10; i++) text += Dec(counters[i]) + (i == 9 ? "}" : ",");
      return text;
    }
  };

  Snap Snapshot() const {
    Snap s;
    s.occ = dut_->o_occ;
    s.occ_count[0] = dut_->o_occ_count_alu;
    s.occ_count[1] = dut_->o_occ_count_md;
    s.occ_count[2] = dut_->o_occ_count_wb;
    s.occ_count[3] = dut_->o_occ_count_net;
    s.live = dut_->o_lease_live;
    s.used = dut_->o_lease_used;
    s.end_kind = dut_->o_lease_end_kind;
    s.rr = dut_->o_rr;
    s.gen = dut_->o_lease_gen;
    s.mask = dut_->o_lease_mask;
    s.slot = dut_->o_lease_slot;
    s.counters[0] = dut_->o_grant_count;
    s.counters[1] = dut_->o_rel_ok_count;
    s.counters[2] = dut_->o_can_ok_count;
    s.counters[3] = dut_->o_rel_stale_count;
    s.counters[4] = dut_->o_can_stale_count;
    s.counters[5] = dut_->o_rel_repeat_count;
    s.counters[6] = dut_->o_can_repeat_count;
    s.counters[7] = dut_->o_rel_after_cancel_count;
    s.counters[8] = dut_->o_can_after_release_count;
    s.counters[9] = dut_->o_live_count;
    return s;
  }

 private:
  void Drive(const Stim& s, bool rst) {
    dut_->rst = rst ? 1 : 0;
    uint32_t valid = 0;
    uint32_t mask = 0;
    for (uint32_t i = 0; i < g_.req_count; i++) {
      if (s.req_valid[i]) valid |= (1u << i);
      mask |= (s.req_mask[i] & 0xf) << (i * g_.pools);
    }
    dut_->req_valid = valid;
    dut_->req_mask = mask;
    dut_->rel_valid = s.rel_valid ? 1 : 0;
    dut_->rel_id = Encode(s.rel);
    dut_->can_valid = s.can_valid ? 1 : 0;
    dut_->can_id = Encode(s.can);
  }

  uint32_t Encode(const LeaseId& id) const {
    return (id.gen & GenMask(g_.gen_w)) | ((id.row & 0x7u) << g_.gen_w);
  }

  void CompareComb(const ShadowLease::Out& e, const Stim& s, const std::string& where) {
    for (uint32_t i = 0; i < g_.req_count; i++) {
      const bool dut_ready = Field(dut_->req_ready, i, 1) != 0;
      if (dut_ready != (e.ready[i] != 0)) {
        Fail(where, "stimulus " + s.str() + ": req_ready[" + Dec(i) + "] is " + Bool(dut_ready) +
                        ", the contract says " + Bool(e.ready[i] != 0) +
                        " (round-robin pointer " + Dec(shadow_->rr()) + ", DUT state " +
                        Snapshot().str() + ")");
      }
      const uint32_t raw = Field(dut_->grant_id, i * g_.lease_w, g_.lease_w);
      const LeaseId got{Field(raw, g_.gen_w, g_.idx_w), Field(raw, 0, g_.gen_w)};
      if (got != e.id[i]) {
        Fail(where, "stimulus " + s.str() + ": grant_id[" + Dec(i) + "] is " + got.str() +
                        ", the contract says " + e.id[i].str());
      }
      for (uint32_t p = 0; p < g_.pools; p++) {
        const uint32_t got_slot = Field(dut_->grant_slot, (i * g_.pools + p) * g_.slot_w, g_.slot_w);
        if (got_slot != e.slot[i * g_.pools + p]) {
          Fail(where, "stimulus " + s.str() + ": grant_slot[" + Dec(i) + "," +
                          std::string(PoolName(p)) + "] is " + Dec(got_slot) +
                          ", the contract says " + Dec(e.slot[i * g_.pools + p]));
        }
      }
    }
    struct Pulse {
      const char* name;
      bool dut;
      bool want;
    };
    const Pulse pulses[] = {
        {"rel_ok", dut_->rel_ok != 0, e.rel_ok != 0},
        {"rel_stale", dut_->rel_stale != 0, e.rel_stale != 0},
        {"rel_repeat", dut_->rel_repeat != 0, e.rel_repeat != 0},
        {"rel_after_cancel", dut_->rel_after_cancel != 0, e.rel_after_cancel != 0},
        {"can_ok", dut_->can_ok != 0, e.can_ok != 0},
        {"can_stale", dut_->can_stale != 0, e.can_stale != 0},
        {"can_repeat", dut_->can_repeat != 0, e.can_repeat != 0},
        {"can_after_release", dut_->can_after_release != 0, e.can_after_release != 0},
    };
    for (const Pulse& p : pulses) {
      if (p.dut != p.want) {
        Fail(where, "stimulus " + s.str() + ": " + std::string(p.name) + " is " + Bool(p.dut) +
                        ", the contract says " + Bool(p.want));
      }
    }
  }

  void CompareState(const std::string& where) {
    const ShadowLease& sh = *shadow_;
    for (uint32_t p = 0; p < g_.pools; p++) {
      for (uint32_t sl = 0; sl < g_.max_sz; sl++) {
        const uint32_t got = occ_bit(p, sl);
        const uint32_t want = (sl < g_.size[p]) ? sh.occ_bit(p, sl) : 0u;
        if (got != want) {
          Fail(where, "o_occ slot " + Dec(sl) + " of pool " + PoolName(p) + " is " + Dec(got) +
                          ", the ledger says " + Dec(want));
        }
      }
      const uint32_t got_count = occ_count(p);
      if (got_count != sh.occ_count(p)) {
        Fail(where, "o_occ_count[" + std::string(PoolName(p)) + "] is " + Dec(got_count) +
                        ", the ledger says " + Dec(sh.occ_count(p)));
      }
    }
    for (uint32_t r = 0; r < g_.leases; r++) {
      const bool live = Field(dut_->o_lease_live, r, 1) != 0;
      const bool used = Field(dut_->o_lease_used, r, 1) != 0;
      const bool endk = Field(dut_->o_lease_end_kind, r, 1) != 0;
      if (live != sh.live()[r]) {
        Fail(where, "o_lease_live[" + Dec(r) + "] is " + Bool(live) + ", the ledger says " +
                        Bool(sh.live()[r]));
      }
      if (used != sh.used()[r]) {
        Fail(where, "o_lease_used[" + Dec(r) + "] is " + Bool(used) + ", the ledger says " +
                        Bool(sh.used()[r]));
      }
      if (used && endk != sh.epoch_cancelled(r)) {
        Fail(where, "o_lease_end_kind[" + Dec(r) + "] is " + Bool(endk) +
                        ", the ledger says the epoch ended " +
                        (sh.epoch_cancelled(r) ? "cancelled" : "released"));
      }
      // The generation array is not reset, so it is compared only where `used`
      // gives it meaning. The generation a grant *hands out* is compared on
      // every grant through grant_id, including the first to a record.
      if (used && lease_gen(r) != sh.gen(r)) {
        Fail(where, "o_lease_gen[" + Dec(r) + "] is " + Dec(lease_gen(r)) + ", the ledger says " +
                        Dec(sh.gen(r)));
      }
      if (live) {
        for (uint32_t p = 0; p < g_.pools; p++) {
          const uint32_t got_mask = Field(dut_->o_lease_mask, r * g_.pools + p, 1);
          const uint32_t want_mask = (sh.mask_of(r) >> p) & 1u;
          if (got_mask != want_mask) {
            Fail(where, "o_lease_mask[" + Dec(r) + "," + PoolName(p) + "] is " + Dec(got_mask) +
                            ", the ledger says " + Dec(want_mask));
          }
          if (got_mask) {
            const uint32_t got_slot = lease_slot(r, p);
            if (got_slot != sh.slots_of(r, p)) {
              Fail(where, "o_lease_slot[" + Dec(r) + "," + PoolName(p) + "] is " + Dec(got_slot) +
                              ", the ledger says " + Dec(sh.slots_of(r, p)));
            }
          }
        }
      }
    }
    if (rr() != sh.rr()) {
      Fail(where, "o_rr is " + Dec(rr()) + ", the shadow's round-robin pointer is " + Dec(sh.rr()));
    }
    struct Counter {
      const char* name;
      uint32_t dut;
      uint64_t want;
    };
    const Counter counters[] = {
        {"o_grant_count", dut_->o_grant_count, sh.grant_count()},
        {"o_rel_ok_count", dut_->o_rel_ok_count, sh.rel_ok()},
        {"o_can_ok_count", dut_->o_can_ok_count, sh.can_ok()},
        {"o_rel_stale_count", dut_->o_rel_stale_count, sh.rel_stale()},
        {"o_can_stale_count", dut_->o_can_stale_count, sh.can_stale()},
        {"o_rel_repeat_count", dut_->o_rel_repeat_count, sh.rel_repeat()},
        {"o_can_repeat_count", dut_->o_can_repeat_count, sh.can_repeat()},
        {"o_rel_after_cancel_count", dut_->o_rel_after_cancel_count, sh.rel_after_can()},
        {"o_can_after_release_count", dut_->o_can_after_release_count, sh.can_after_rel()},
        {"o_live_count", dut_->o_live_count, sh.live_count()},
    };
    for (const Counter& c : counters) {
      if (c.dut != c.want) {
        Fail(where, std::string(c.name) + " is " + Dec(c.dut) + ", the ledger says " + Dec(c.want));
      }
    }
  }

  // Invariants checked against the DUT alone, with no reference to the shadow.
  // These are the conservation and all-or-none statements the card is about, and
  // they hold on every cycle of every phase, including the random soak.
  void CheckStandingInvariants(const std::string& where) {
    const uint32_t grants = dut_->o_grant_count;
    const uint32_t rel = dut_->o_rel_ok_count;
    const uint32_t can = dut_->o_can_ok_count;
    const uint32_t live = dut_->o_live_count;
    if (grants != rel + can + live) {
      Fail(where, "conservation broken in the DUT: " + Dec(grants) + " grants != " + Dec(rel) +
                      " releases + " + Dec(can) + " cancels + " + Dec(live) + " live");
    }
    uint32_t popcount = 0;
    for (uint32_t r = 0; r < g_.leases; r++) {
      popcount += Field(dut_->o_lease_live, r, 1);
    }
    if (popcount != live) {
      Fail(where, "o_live_count (" + Dec(live) +
                      ") does not equal the population of o_lease_live (" + Dec(popcount) + ")");
    }
    for (uint32_t p = 0; p < g_.pools; p++) {
      uint32_t occupied = 0;
      for (uint32_t sl = 0; sl < g_.size[p]; sl++) {
        occupied += occ_bit(p, sl);
      }
      if (occupied != occ_count(p)) {
        Fail(where, "pool " + std::string(PoolName(p)) + " has " + Dec(occupied) +
                        " slots set in o_occ but o_occ_count says " + Dec(occ_count(p)));
      }
    }
  }

  Vmosaic_lease_alloc_tb* dut_;
  mosaic::ClockDriver* clk_;
  ShadowLease* shadow_ = nullptr;
  uint64_t max_cycles_;
  uint64_t cycles_ = 0;
  uint64_t comparisons_ = 0;
  std::string phase_;
  Geom g_;
};

// ---------------------------------------------------------------- helpers
void RequireStateUnchanged(Harness* h, const Harness::Snap& before, const std::string& where,
                           const std::string& what) {
  const Harness::Snap after = h->Snapshot();
  if (after != before && !after.SameResources(before)) {
    Fail(where, what + ": the ledger was changed by an event that must move no resource\n  before: " +
                    before.str() + "\n  after:  " + after.str());
  }
}

Stim ReqOnly(const Geom& g, uint32_t port, uint32_t mask) {
  Stim s = MakeStim(g);
  AddReq(&s, port, mask);
  return s;
}

Stim TermOnly(const Geom& g, bool release, const LeaseId& id) {
  Stim s = MakeStim(g);
  if (release) {
    s.rel_valid = true;
    s.rel = id;
  } else {
    s.can_valid = true;
    s.can = id;
  }
  return s;
}

// ============================================================================
// Phase 1: the cold ledger, and a reset taken while leases are live.
// ============================================================================
void PhaseResetState(Harness* h, mosaic::Reporter* reporter) {
  const Geom& g = h->shadow()->geom();
  h->Reset(4);
  const Harness::Snap cold = h->Snapshot();
  Require(cold.occ == 0, "reset-state", "a pool slot is occupied after reset");
  Require(cold.live == 0 && cold.used == 0, "reset-state",
          "a lease record is live or used after reset");
  Require(cold.rr == 0, "reset-state", "the round-robin pointer is not zero after reset");
  for (int i = 0; i < 10; i++) {
    Require(cold.counters[i] == 0, "reset-state",
            "counter " + Dec(i) + " is " + Dec(cold.counters[i]) + " after reset");
  }

  // A release for a record that has never been granted is stale, and it changes
  // nothing -- including that it must not make the record look used.
  ShadowLease::Out o = h->Cycle(TermOnly(g, true, LeaseId{0, 0}));
  Require(o.rel_stale != 0, "reset-state",
          "a release for a never-granted lease id was not reported stale");
  RequireStateUnchanged(h, cold, "reset-state", "a stale release on a cold ledger");

  // Now take a reset while leases are live, with every pool occupied.
  h->Reset(4);
  std::vector<LeaseId> ids;
  const uint32_t masks[3] = {MASK_ALU | MASK_WB, MASK_MD | MASK_WB, MASK_NET | MASK_WB};
  for (uint32_t i = 0; i < 3; i++) {
    ShadowLease::Out got = h->Cycle(ReqOnly(g, i, masks[i]));
    Require(got.ready[i] != 0, "reset-state", "a cold-ledger grant was refused");
    ids.push_back(got.id[i]);
  }
  Require(h->occ_count(POOL_ALU) == 1u && h->occ_count(POOL_MD) == 1u &&
              h->occ_count(POOL_NET) == 1u && h->occ_count(POOL_WB) == 3u,
          "reset-state", "three grants did not occupy the classes they named");

  h->Reset(4);
  const Harness::Snap after = h->Snapshot();
  Require(after == cold, "reset-state",
          "a reset taken while leases were live did not restore the cold ledger:\n  cold:  " +
              cold.str() + "\n  after: " + after.str());

  // The pre-reset ids are dead: the `used` bits and the generations were
  // re-established together, so every one of them is stale rather than able to
  // free someone else's resources.
  for (const LeaseId& id : ids) {
    ShadowLease::Out o2 = h->Cycle(TermOnly(g, true, id));
    Require(o2.rel_stale != 0, "reset-state",
            "a pre-reset lease id " + id.str() + " was not rejected after the reset");
    RequireStateUnchanged(h, cold, "reset-state", "a pre-reset lease id after a reset");
    ShadowLease::Out o3 = h->Cycle(TermOnly(g, false, id));
    Require(o3.can_stale != 0, "reset-state",
            "a pre-reset lease id " + id.str() + " was not rejected as a cancel after the reset");
  }

  reporter->Check(true,
                  "reset-state: the cold ledger is exactly the documented one, and a reset taken "
                  "with every pool occupied restores it and leaves no pre-reset id able to move a "
                  "resource");
}

// ============================================================================
// Phase 2: two requesters, one shared MUL/DIV slot.
// ============================================================================
void PhaseConflict(Harness* h, mosaic::Reporter* reporter) {
  const Geom& g = h->shadow()->geom();
  h->Reset(4);
  Require(g.size[POOL_MD] == 1u, "conflict",
          "the shared MUL/DIV pool is not one slot, so this phase would not be testing contention");

  Stim both = MakeStim(g);
  AddReq(&both, 0, MASK_MD | MASK_WB);
  AddReq(&both, 1, MASK_MD | MASK_WB);
  ShadowLease::Out first = h->Cycle(both);
  Require(first.ready[0] != 0 && first.ready[1] == 0, "conflict",
          "both clusters asked for the one shared MUL/DIV slot and port 0 did not win: the "
          "documented policy is that the candidate order starts at the round-robin pointer, which "
          "is 0 after reset");
  Require(h->occ_count(POOL_MD) == 1u, "conflict",
          "exactly one MUL/DIV slot must be occupied by the winner, not " +
              Dec(h->occ_count(POOL_MD)));

  // The loser keeps asking, alone. It must be refused while the winner holds the
  // slot, and the refusal must not change anything at all.
  const Stim loser = ReqOnly(g, 1, MASK_MD | MASK_WB);
  ShadowLease::Out refused = h->Cycle(loser);
  Require(refused.ready[1] == 0, "conflict",
          "the loser was granted while the shared MUL/DIV slot was still held");
  const Harness::Snap held = h->Snapshot();
  h->Cycle(loser);
  RequireStateUnchanged(h, held, "conflict", "a refused request with a contended slot");

  // The winner releases. The returned slot must fund the loser's grant on the
  // same edge.
  Stim release_and_ask = loser;
  release_and_ask.rel_valid = true;
  release_and_ask.rel = first.id[0];
  ShadowLease::Out granted = h->Cycle(release_and_ask);
  Require(granted.rel_ok != 0, "conflict", "the winner's release was not accepted");
  Require(granted.ready[1] != 0, "conflict",
          "the loser was not granted on the edge that returned the shared slot: a credit returned "
          "on an edge must be spendable on that edge");

  reporter->Check(true,
                  "conflict: two requesters contesting the single shared MUL/DIV slot produced "
                  "exactly one grant, the refusal changed nothing, and the loser was granted on the "
                  "edge the winner's release returned the slot");
}

// ============================================================================
// Phase 3: all-or-none. The card's "reserve the FU and wait forever for WB".
// ============================================================================
void PhasePartial(Harness* h, mosaic::Reporter* reporter) {
  const Geom& g = h->shadow()->geom();
  h->Reset(4);

  // Fill the result-credit pool with leases that name nothing else, one per
  // cycle: each is a result credit held with no FU slot.
  std::vector<LeaseId> credits;
  for (uint32_t i = 0; i < g.size[POOL_WB]; i++) {
    ShadowLease::Out o = h->Cycle(ReqOnly(g, 2, MASK_WB));
    Require(o.ready[2] != 0, "partial",
            "credit " + Dec(i) + " of " + Dec(g.size[POOL_WB]) + " was refused with the pool empty");
    credits.push_back(o.id[2]);
  }
  Require(h->occ_count(POOL_WB) == g.size[POOL_WB], "partial", "the credit pool is not full");
  Require(h->occ_count(POOL_ALU) == 0u, "partial", "an ALU slot was taken by a credit-only lease");

  // A request that needs the ALU *and* a credit: the ALU is free, the credit is
  // not. Nothing may be granted, and the ALU slot must stay free.
  const Stim mixed = ReqOnly(g, 0, MASK_ALU | MASK_WB);
  const Harness::Snap before = h->Snapshot();
  ShadowLease::Out refused = h->Cycle(mixed);
  Require(refused.ready[0] == 0, "partial",
          "a request naming {FU, credit} was granted with no credit free");
  RequireStateUnchanged(h, before, "partial", "a partial-resource refusal");
  Require(h->occ_count(POOL_ALU) == 0u, "partial",
          "the FU slot was reserved for a request that was refused: this is the 'reserve the FU and "
          "wait forever for WB' deadlock the card names as a failure criterion");

  // The ALU slot is still there to be taken, on the same edge the credit comes
  // back: the reservation is not merely invisible, it does not exist.
  Stim funded = mixed;
  funded.rel_valid = true;
  funded.rel = credits[0];
  ShadowLease::Out granted = h->Cycle(funded);
  Require(granted.ready[0] != 0, "partial",
          "the {FU, credit} request was still refused after a credit was returned in the same cycle");
  Require(h->occ_count(POOL_ALU) == 1u, "partial", "the granted request did not take an FU slot");

  // The mirror image: the FU pool full, a credit free.
  h->Reset(4);
  ShadowLease::Out a0 = h->Cycle(ReqOnly(g, 0, MASK_ALU));
  ShadowLease::Out a1 = h->Cycle(ReqOnly(g, 0, MASK_ALU));
  Require(a0.ready[0] != 0 && a1.ready[0] != 0, "partial",
          "the ALU pool did not yield its " + Dec(g.size[POOL_ALU]) + " slots");
  const Harness::Snap full_fu = h->Snapshot();
  ShadowLease::Out refused2 = h->Cycle(ReqOnly(g, 1, MASK_ALU | MASK_WB));
  Require(refused2.ready[1] == 0, "partial", "a request was granted with the FU pool full");
  Require(h->occ_count(POOL_WB) == 0u, "partial",
          "a WB credit was consumed by a request that was refused for an FU slot");
  RequireStateUnchanged(h, full_fu, "partial", "a refusal with the FU pool full");

  reporter->Check(true,
                  "partial: a request whose set was not wholly available was refused, and neither "
                  "the free class nor the scarce one was touched -- a partial reservation is not "
                  "representable in this design, which is the form the card's Fail criterion takes "
                  "here");
}

// ============================================================================
// Phase 4: the flush path.
// ============================================================================
void PhaseCancel(Harness* h, mosaic::Reporter* reporter) {
  const Geom& g = h->shadow()->geom();
  h->Reset(4);

  // Keep records 0..2 busy so that the lease which is cancelled holds the only
  // free record, and the request that follows must reuse it.
  std::vector<LeaseId> keep;
  for (uint32_t i = 0; i + 1 < g.leases; i++) {
    ShadowLease::Out o = h->Cycle(ReqOnly(g, 2, MASK_WB));
    Require(o.ready[2] != 0, "cancel", "a keep-alive grant was refused");
    keep.push_back(o.id[2]);
  }

  ShadowLease::Out got = h->Cycle(ReqOnly(g, 0, MASK_ALU | MASK_WB));
  Require(got.ready[0] != 0, "cancel", "the lease to be cancelled was refused");
  const LeaseId victim = got.id[0];
  Require(victim.row + 1 == g.leases, "cancel",
          "the cancelled lease did not take the only free record, so the phase cannot show that "
          "the old id becomes stale when the record is reused");
  Require(h->occ_count(POOL_ALU) == 1u && h->occ_count(POOL_WB) == g.size[POOL_WB],
          "cancel", "the granted lease did not hold both of the classes it named");

  // The flush.
  ShadowLease::Out cancel = h->Cycle(TermOnly(g, false, victim));
  Require(cancel.can_ok != 0, "cancel", "the flush was not accepted for a live lease");
  Require(h->occ_count(POOL_ALU) == 0u, "cancel",
          "the cancelled lease's FU slot was not returned");
  Require(h->occ_count(POOL_WB) == g.size[POOL_WB] - 1u, "cancel",
          "the cancelled lease's credit was not returned exactly once");

  // The same requester asks again. Only the cancelled record is free, so the new
  // epoch must land on it, with a new generation.
  ShadowLease::Out again = h->Cycle(ReqOnly(g, 0, MASK_ALU | MASK_WB));
  Require(again.ready[0] != 0, "cancel", "the request after the flush was refused");
  Require(again.id[0].row == victim.row, "cancel",
          "the request after the flush did not reuse the freed record");
  Require(again.id[0].gen != victim.gen, "cancel",
          "the reused record was granted with the same generation, so the two ids are not "
          "distinguishable");

  // The old id names a superseded epoch of a record that is live again: it must
  // be rejected as stale, and must not free the new lease's resources.
  const Harness::Snap live_again = h->Snapshot();
  ShadowLease::Out stale = h->Cycle(TermOnly(g, true, victim));
  Require(stale.rel_stale != 0, "cancel",
          "the release of a lease whose record has since been reused was not reported stale");
  RequireStateUnchanged(h, live_again, "cancel", "a stale release aimed at a superseded epoch");
  ShadowLease::Out stale_can = h->Cycle(TermOnly(g, false, victim));
  Require(stale_can.can_stale != 0, "cancel", "the same stale id was not rejected as a cancel");

  // And the live lease still ends exactly once, by cancel.
  ShadowLease::Out end1 = h->Cycle(TermOnly(g, false, again.id[0]));
  Require(end1.can_ok != 0, "cancel", "the new lease could not be cancelled");
  const Harness::Snap ended = h->Snapshot();
  ShadowLease::Out end2 = h->Cycle(TermOnly(g, false, again.id[0]));
  Require(end2.can_repeat != 0, "cancel",
          "a second cancel of the same lease was not reported as a repeat");
  RequireStateUnchanged(h, ended, "cancel", "a repeated cancel");

  reporter->Check(true,
                  "cancel: a flush returned the FU slot and the credit exactly once, the requester "
                  "was served again on a new epoch of the same record, and the old id was rejected "
                  "as a superseded epoch rather than freeing the new lease's resources");
}

// ============================================================================
// Phase 5: the rejection matrix.
// ============================================================================
void PhaseTerminalErrors(Harness* h, mosaic::Reporter* reporter) {
  const Geom& g = h->shadow()->geom();
  h->Reset(4);

  ShadowLease::Out a = h->Cycle(ReqOnly(g, 0, MASK_ALU | MASK_WB));
  const LeaseId id_a = a.id[0];
  ShadowLease::Out b = h->Cycle(ReqOnly(g, 1, MASK_NET | MASK_WB));
  const LeaseId id_b = b.id[1];
  Require(a.ready[0] != 0 && b.ready[1] != 0, "terminal-errors", "setup grants were refused");

  // 1. A double release. The second one is a repeat: counted, and it must not
  //    return the resources a second time.
  ShadowLease::Out r1 = h->Cycle(TermOnly(g, true, id_a));
  Require(r1.rel_ok != 0, "terminal-errors", "the first release was not accepted");
  const Harness::Snap after_first = h->Snapshot();
  ShadowLease::Out r2 = h->Cycle(TermOnly(g, true, id_a));
  Require(r2.rel_repeat != 0, "terminal-errors",
          "a second release of the same lease was not reported as a repeat");
  RequireStateUnchanged(h, after_first, "terminal-errors", "a double release");

  // 2. A lease id that was never granted: a record the design has never used.
  const Harness::Snap after_second = h->Snapshot();
  ShadowLease::Out r3 = h->Cycle(TermOnly(g, true, LeaseId{2, 0}));
  Require(r3.rel_stale != 0, "terminal-errors",
          "a release for a never-granted record was not reported stale");
  RequireStateUnchanged(h, after_second, "terminal-errors", "a never-granted lease id");

  // 3. A record index that does not exist. The index field is one bit wider than
  //    the ledger on purpose, so this is reachable.
  const Harness::Snap after_third = h->Snapshot();
  ShadowLease::Out r4 = h->Cycle(TermOnly(g, true, LeaseId{g.leases, 0}));
  Require(r4.rel_stale != 0, "terminal-errors",
          "a release naming record " + Dec(g.leases) + " of " + Dec(g.leases) +
              " was not reported stale: an unknown index must not alias onto record 0");
  RequireStateUnchanged(h, after_third, "terminal-errors", "an out-of-range record index");

  // 4. A never-issued generation aimed at a *live* lease. This is the case that
  //    would free someone else's resources if the generation were ignored.
  const Harness::Snap id_b_live = h->Snapshot();
  const LeaseId wrong{id_b.row, (id_b.gen + 1u) & h->shadow()->gen_mask()};
  ShadowLease::Out r5 = h->Cycle(TermOnly(g, true, wrong));
  Require(r5.rel_stale != 0, "terminal-errors",
          "a release carrying a generation that was never issued for a live lease was not reported "
          "stale");
  RequireStateUnchanged(h, id_b_live, "terminal-errors",
                        "a stale release aimed at a live lease");

  // 5. Cancel after release, and the crossing cases in the other direction.
  ShadowLease::Out c1 = h->Cycle(TermOnly(g, false, id_a));
  Require(c1.can_after_release != 0, "terminal-errors",
          "a cancel of a released lease was not reported as a cancel-after-release");
  ShadowLease::Out c2 = h->Cycle(TermOnly(g, false, id_b));
  Require(c2.can_ok != 0, "terminal-errors", "the cancel of a live lease was not accepted");
  ShadowLease::Out c3 = h->Cycle(TermOnly(g, false, id_b));
  Require(c3.can_repeat != 0, "terminal-errors", "a repeated cancel was not reported");
  ShadowLease::Out r6 = h->Cycle(TermOnly(g, true, id_b));
  Require(r6.rel_after_cancel != 0, "terminal-errors",
          "a release of a cancelled lease was not reported as a release-after-cancel");

  // The directed counts, read from the DUT rather than from the shadow, so the
  // phase states what the hardware counted.
  Require(h->Snapshot().counters[5] == 1u, "terminal-errors",
          "o_rel_repeat_count is " + Dec(h->Snapshot().counters[5]) + ", expected 1");
  Require(h->Snapshot().counters[8] == 1u, "terminal-errors",
          "o_can_after_release_count is " + Dec(h->Snapshot().counters[8]) + ", expected 1");
  Require(h->Snapshot().counters[6] == 1u, "terminal-errors",
          "o_can_repeat_count is " + Dec(h->Snapshot().counters[6]) + ", expected 1");
  Require(h->Snapshot().counters[7] == 1u, "terminal-errors",
          "o_rel_after_cancel_count is " + Dec(h->Snapshot().counters[7]) + ", expected 1");
  Require(h->Snapshot().counters[3] == 3u, "terminal-errors",
          "o_rel_stale_count is " + Dec(h->Snapshot().counters[3]) +
              ", expected 3 (never granted, out of range, never-issued generation)");

  reporter->Check(true,
                  "terminal-errors: a double release, a never-granted id, an out-of-range record, a "
                  "never-issued generation on a live lease, a cancel-after-release and a "
                  "release-after-cancel were each counted by name, and none of them moved a "
                  "resource");
}

// ============================================================================
// Phase 6: the same-cycle order.
// ============================================================================
void PhaseSameCycle(Harness* h, mosaic::Reporter* reporter) {
  const Geom& g = h->shadow()->geom();
  h->Reset(4);

  std::vector<LeaseId> credits;
  for (uint32_t i = 0; i < g.size[POOL_WB]; i++) {
    credits.push_back(h->Cycle(ReqOnly(g, 2, MASK_WB)).id[2]);
  }
  const uint32_t freed_slot = h->lease_slot(credits[2].row, POOL_WB);

  // Without a terminal in the cycle, the request is refused.
  const Stim ask = ReqOnly(g, 0, MASK_ALU | MASK_WB);
  ShadowLease::Out refused = h->Cycle(ask);
  Require(refused.ready[0] == 0, "same-cycle",
          "the {FU, credit} request was granted while every credit was held");

  // With the release in the same cycle, it is granted, and it takes the slot the
  // release returned.
  Stim funded = ask;
  funded.rel_valid = true;
  funded.rel = credits[2];
  ShadowLease::Out granted = h->Cycle(funded);
  Require(granted.rel_ok != 0, "same-cycle", "the release was not accepted in the funded cycle");
  Require(granted.ready[0] != 0, "same-cycle",
          "a credit returned on this edge did not fund a grant on this edge");
  Require(granted.slot[0 * g.pools + POOL_WB] == freed_slot, "same-cycle",
          "the grant took credit slot " + Dec(granted.slot[0 * g.pools + POOL_WB]) + ", but the "
          "release returned slot " + Dec(freed_slot) + " and the scan takes the lowest free slot");

  reporter->Check(true,
                  "same-cycle: a credit returned on an edge was spendable on that edge, on the "
                  "slot the release returned");
}

// ============================================================================
// Phase 7: replay -- a refused request must leave no trace.
// ============================================================================
void PhaseRefusalReplay(Harness* h, mosaic::Reporter* reporter) {
  const Geom& g = h->shadow()->geom();
  h->Reset(4);

  // Script: fill the credit pool, then offer a request that cannot be served,
  // then release the credits and serve the request.
  const uint32_t kRefused = g.size[POOL_WB];
  std::vector<Stim> script;
  for (uint32_t i = 0; i < g.size[POOL_WB]; i++) {
    script.push_back(ReqOnly(g, 2, MASK_WB));
  }
  script.push_back(ReqOnly(g, 0, MASK_ALU | MASK_WB));  // index kRefused

  // Discovery: run the prefix and record the ids the grants handed out, so the
  // tail's terminal events name real leases without this file assuming them.
  std::vector<LeaseId> ids;
  for (uint32_t i = 0; i <= kRefused; i++) {
    ShadowLease::Out o = h->Cycle(script[i]);
    if (i < kRefused) {
      ids.push_back(o.id[2]);
    } else {
      Require(o.ready[0] == 0, "refusal",
              "the request at cycle " + Dec(kRefused) +
                  " was expected to be refused (the credit pool is full), so the phase would not "
                  "be testing a refusal at all");
    }
  }
  for (const LeaseId& id : ids) {
    script.push_back(TermOnly(g, true, id));
  }
  script.push_back(ReqOnly(g, 2, MASK_ALU | MASK_WB));

  // Run A: the script as written.
  h->Reset(4);
  std::vector<Harness::Snap> snaps_a;
  for (const Stim& s : script) {
    h->Cycle(s);
    snaps_a.push_back(h->Snapshot());
  }

  // Run B: the same script with the refused request never offered.
  h->Reset(4);
  std::vector<Harness::Snap> snaps_b;
  for (uint32_t i = 0; i < script.size(); i++) {
    Stim s = script[i];
    if (i == kRefused) s.req_valid[0] = false;
    h->Cycle(s);
    snaps_b.push_back(h->Snapshot());
  }

  // From the refusal onward the two runs must be identical, cycle for cycle.
  Require(snaps_a.size() == snaps_b.size(), "refusal", "the replay scripts differ in length");
  for (uint32_t i = kRefused; i < script.size(); i++) {
    if (snaps_a[i] != snaps_b[i]) {
      Fail("refusal",
           "cycle " + Dec(i) + " differs between the run that offered a refused request at cycle " +
               Dec(kRefused) + " and the run that never offered it:\n  offered: " +
               snaps_a[i].str() + "\n  absent:  " + snaps_b[i].str());
    }
  }

  reporter->Check(true,
                  "refusal: " + Dec(script.size() - kRefused) +
                      " cycles after a refused request were bit-identical to the same stimulus "
                      "with the request never offered");
}

// ============================================================================
// Phase 8: the generation wrap.
// ============================================================================
void PhaseWrap(Harness* h, mosaic::Reporter* reporter) {
  const Geom& g = h->shadow()->geom();
  h->Reset(4);

  // Keep every record but the last busy, so the looping grant must land on the
  // last one and its generation is the only one moving.
  for (uint32_t i = 0; i + 1 < g.leases; i++) {
    ShadowLease::Out o = h->Cycle(ReqOnly(g, 2, MASK_WB));
    Require(o.ready[2] != 0, "wrap", "a keep-alive grant was refused");
  }

  const uint32_t iterations = (1u << g.gen_w) + 16u;
  uint32_t wraps = 0;
  uint32_t previous_gen = 0;
  for (uint32_t i = 0; i < iterations; i++) {
    ShadowLease::Out got = h->Cycle(ReqOnly(g, 0, MASK_ALU | MASK_WB));
    Require(got.ready[0] != 0, "wrap", "iteration " + Dec(i) + " was refused");
    Require(got.id[0].row + 1 == g.leases, "wrap",
            "the looping grant did not take the last record");
    if (i > 0 && got.id[0].gen <= previous_gen) wraps++;
    previous_gen = got.id[0].gen;
    ShadowLease::Out end = h->Cycle(TermOnly(g, true, got.id[0]));
    Require(end.rel_ok != 0, "wrap", "iteration " + Dec(i) + "'s release was not accepted");
  }
  Require(wraps >= 1, "wrap",
          "the generation did not wrap over " + Dec(iterations) + " grants; the modulus is 2**" +
              Dec(g.gen_w));

  reporter->Check(true,
                  "wrap: one record's generation was counted through its whole modulus (" +
                      Dec(iterations) + " grants, " + Dec(wraps) + " wrap(s)), and the ledger and "
                      "the DUT agreed on every step including across the wrap");
}

// ============================================================================
// Phase 9: the random soak.
// ============================================================================
struct Coverage {
  uint32_t grants = 0;
  uint32_t releases = 0;
  uint32_t cancels = 0;
  uint32_t stale = 0;
  uint32_t repeat = 0;
  uint32_t crossed = 0;
  uint32_t conflicts = 0;       // >= 2 requesters valid, at least one refused
  uint32_t same_cycle = 0;      // a terminal accepted and a grant in the same cycle
  uint32_t full_house = 0;      // every pool's every slot occupied at once
  uint32_t rr_advanced = 0;
  uint32_t gen_advanced = 0;
};

LeaseId RandomTarget(mosaic::Rng* rng, ShadowLease* sh, const Geom& g) {
  const uint32_t roll = rng->Below(100);
  const std::vector<uint32_t> live = sh->live_rows();
  const std::vector<uint32_t> dead = sh->dead_used_rows();
  if (roll < 45 && !live.empty()) {
    const uint32_t row = live[rng->Below(static_cast<uint32_t>(live.size()))];
    return LeaseId{row, sh->gen(row)};  // a real, current epoch: accepted
  }
  if (roll < 60 && !dead.empty()) {
    const uint32_t row = dead[rng->Below(static_cast<uint32_t>(dead.size()))];
    return LeaseId{row, sh->gen(row)};  // terminated epoch: repeat or crossing
  }
  if (roll < 75 && !live.empty()) {
    const uint32_t row = live[rng->Below(static_cast<uint32_t>(live.size()))];
    // A generation that was never issued for this record.
    return LeaseId{row, (sh->gen(row) + 1u + rng->Below(3)) & sh->gen_mask()};
  }
  if (roll < 90) {
    return LeaseId{rng->Below(g.leases), rng->Below(1u << g.gen_w)};  // often stale
  }
  // An index the ledger does not have.
  return LeaseId{g.leases + rng->Below(4), rng->Below(1u << g.gen_w)};
}

void PhaseRandom(Harness* h, mosaic::Reporter* reporter, uint32_t seed, uint32_t cycles) {
  const Geom& g = h->shadow()->geom();
  mosaic::Rng rng(seed);
  Coverage cov;
  h->Reset(4);

  for (uint32_t cycle = 0; cycle < cycles; cycle++) {
    Stim s = MakeStim(g);
    uint32_t offered = 0;
    for (uint32_t i = 0; i < g.req_count; i++) {
      if (rng.Chance(45)) {
        AddReq(&s, i, rng.Below(1u << g.pools));
        offered++;
      }
    }
    if (rng.Chance(35)) {
      s.rel_valid = true;
      s.rel = RandomTarget(&rng, h->shadow(), g);
    }
    if (rng.Chance(35)) {
      s.can_valid = true;
      s.can = RandomTarget(&rng, h->shadow(), g);
    }

    const ShadowLease::Out o = h->Cycle(s);

    uint32_t ready_count = 0;
    for (uint32_t i = 0; i < g.req_count; i++) {
      if (o.ready[i]) {
        ready_count++;
        cov.grants++;
      }
    }
    if (offered >= 2 && ready_count < offered) cov.conflicts++;
    if ((o.rel_ok || o.can_ok) && ready_count > 0) cov.same_cycle++;
    if (o.rel_ok) cov.releases++;
    if (o.can_ok) cov.cancels++;
    if (o.rel_stale || o.can_stale) cov.stale++;
    if (o.rel_repeat || o.can_repeat) cov.repeat++;
    if (o.rel_after_cancel || o.can_after_release) cov.crossed++;

    bool house_full = true;
    for (uint32_t p = 0; p < g.pools; p++) {
      if (h->occ_count(p) != g.size[p]) house_full = false;
    }
    if (house_full) cov.full_house++;
  }

  // Anti-vacuity: a soak in which nothing happened proves nothing. These are
  // demands, not floors that happen to pass.
  Require(cov.grants > 0, "random", "no grant was ever made");
  Require(cov.releases > 0, "random", "no release was ever accepted");
  Require(cov.cancels > 0, "random", "no cancel was ever accepted");
  Require(cov.stale > 0, "random", "no stale lease id was ever rejected");
  Require(cov.repeat > 0, "random", "no repeated terminal event was ever rejected");
  Require(cov.crossed > 0, "random", "no cross-path terminal (cancel after release or the reverse) "
                                      "was ever rejected");
  Require(cov.conflicts > 0, "random", "the soak never produced a contended request");
  Require(cov.same_cycle > 0, "random",
          "the soak never returned a credit on an edge that also granted");
  Require(cov.full_house > 0, "random",
          "the soak never had every pool's every slot occupied at once, so it never exercised the "
          "exhausted case");

  reporter->Check(true,
                  "random: " + Dec(cov.grants) + " grants, " + Dec(cov.releases) + " releases, " +
                      Dec(cov.cancels) + " cancels, " + Dec(cov.stale) + " stale ids, " +
                      Dec(cov.repeat) + " repeats, " + Dec(cov.crossed) + " cross-path events, " +
                      Dec(cov.conflicts) + " contended cycles and " + Dec(cov.same_cycle) +
                      " same-cycle terminal+grant cycles over " + Dec(cycles) + " cycles");
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

  mosaic::Reporter reporter(options, std::string("Verilator ") + Verilated::productVersion());
  mosaic::ClockDriver clk;
  Vmosaic_lease_alloc_tb dut;

  // The geometry readback ports are `assign`s, so they only hold their values
  // after an eval. Every input is driven to a known value first: the runner
  // builds with `--x-initial unique`, and reading a port that no eval has
  // propagated would size the whole shadow from a value the hardware never
  // presented.
  dut.clk = 0;
  dut.rst = 1;
  dut.req_valid = 0;
  dut.req_mask = 0;
  dut.rel_valid = 0;
  dut.rel_id = 0;
  dut.can_valid = 0;
  dut.can_id = 0;
  dut.eval();

  // The geometry comes from the elaborated DUT, never from a literal here.
  Geom g;
  g.req_count = dut.o_req_count;
  g.pools = dut.o_pools;
  g.leases = dut.o_leases;
  g.max_sz = dut.o_max_sz;
  g.gen_w = dut.o_gen_w;
  g.slot_w = dut.o_slot_w;
  g.lease_w = dut.o_lease_w;
  g.size[POOL_ALU] = dut.o_size_alu;
  g.size[POOL_MD] = dut.o_size_md;
  g.size[POOL_WB] = dut.o_size_wb;
  g.size[POOL_NET] = dut.o_size_net;
  g.row_w = 0;
  g.idx_w = 0;

  Harness harness(&dut, &clk, options.max_cycles, g);
  ShadowLease shadow(g);

  std::string detail;
  bool passed = true;
  try {
    harness.Reset(4);

    // The relations this file depends on, asserted before anything else uses
    // them, plus an explicit statement of which profile this case's evidence is
    // for: a profile change fails here rather than producing a weaker test.
    Require(g.pools == 4 && g.req_count == 3 && g.leases == 4, "geometry",
            "this case's evidence is for p0 (3 issue ports, 4 classes, 4 lease records); the DUT "
            "reports " + Dec(g.req_count) + " ports, " + Dec(g.pools) + " classes and " +
            Dec(g.leases) + " records");
    Require(g.size[POOL_ALU] == 2 && g.size[POOL_MD] == 1 && g.size[POOL_WB] == 4 &&
                g.size[POOL_NET] == 2,
            "geometry",
            "the p0 pool sizes are ALU 2, MD 1, WB 4, NET 2; the DUT reports " +
                Dec(g.size[POOL_ALU]) + ", " + Dec(g.size[POOL_MD]) + ", " + Dec(g.size[POOL_WB]) +
                ", " + Dec(g.size[POOL_NET]));
    Require(g.leases == g.size[POOL_WB], "geometry",
            "the lease ledger must be exactly the result-credit pool, because a live lease holds "
            "one credit for its whole life");
    Require(g.max_sz >= g.size[POOL_ALU] && g.max_sz >= g.size[POOL_MD] &&
                g.max_sz >= g.size[POOL_WB] && g.max_sz >= g.size[POOL_NET],
            "geometry", "the pool stride is smaller than a pool");
    Require(g.slot_w == 0 || (1u << g.slot_w) >= g.max_sz, "geometry",
            "the slot index is too narrow for the pool stride");
    Require((1u << g.gen_w) > g.leases, "geometry",
            "the generation modulus does not exceed the number of live leases, so a stale id could "
            "alias immediately");
    Require(g.leases >= 2 && g.req_count >= 2, "geometry",
            "the ledger or the port count is too small to test contention");
    g.row_w = 0;
    for (uint32_t r = 1; r < g.leases; r <<= 1) g.row_w++;
    g.idx_w = g.lease_w - g.gen_w;
    Require(g.idx_w > g.row_w, "geometry",
            "the lease id's index field has no spare encoding, so the out-of-range rejection this "
            "case exercises would be unreachable");

    for (uint32_t p = 0; p < g.pools; p++) {
      Require(g.size[p] >= 1, "geometry", "a pool has no slots");
    }

    // TEMPORARY raw probe (removed before the report).
    {
      harness.Reset(4);
      for (uint32_t mask = 1; mask < 16; mask++) {
        dut.clk = 0; dut.rst = 0; dut.req_valid = 1; dut.req_mask = mask;
        dut.rel_valid = 0; dut.can_valid = 0; dut.eval();
        std::printf("RAW mask=0x%x ready=0x%x gid=0x%x gslot=0x%x occ=0x%x live=0x%x rr=%u dbg0=0x%x dbg1=0x%x\n",
                    mask, (uint32_t)dut.req_ready, (uint32_t)dut.grant_id,
                    (uint32_t)dut.grant_slot, (uint32_t)dut.o_occ, (uint32_t)dut.o_lease_live,
                    (uint32_t)dut.o_rr, (uint32_t)dut.o_dbg0, (uint32_t)dut.o_dbg1);
      }
      std::printf("RAW geometry req=%u pools=%u leases=%u max=%u slot_w=%u gen_w=%u lease_w=%u sizes=%u,%u,%u,%u\n",
                  g.req_count, g.pools, g.leases, g.max_sz, g.slot_w, g.gen_w, g.lease_w,
                  g.size[0], g.size[1], g.size[2], g.size[3]);
    }

    auto fresh = [&]() {
      harness.Reset(4);
      shadow.Reset();
      harness.Bind(&shadow);
    };

    // Phase order is deliberate: the directed mechanisms run first and the soak
    // last, because the soak fails on "some cycle" and would mask the phases
    // that can say which structure is at fault.
    fresh();
    harness.Phase("reset-state");
    PhaseResetState(&harness, &reporter);

    fresh();
    harness.Phase("conflict");
    PhaseConflict(&harness, &reporter);

    fresh();
    harness.Phase("partial");
    PhasePartial(&harness, &reporter);

    fresh();
    harness.Phase("cancel");
    PhaseCancel(&harness, &reporter);

    fresh();
    harness.Phase("terminal-errors");
    PhaseTerminalErrors(&harness, &reporter);

    fresh();
    harness.Phase("same-cycle");
    PhaseSameCycle(&harness, &reporter);

    fresh();
    harness.Phase("refusal");
    PhaseRefusalReplay(&harness, &reporter);

    fresh();
    harness.Phase("wrap");
    PhaseWrap(&harness, &reporter);

    fresh();
    harness.Phase("random");
    PhaseRandom(&harness, &reporter, static_cast<uint32_t>(options.seed), 4000);

    detail = "lease contract holds: " + std::to_string(harness.comparisons()) +
             " shadow comparisons over " + std::to_string(harness.cycles()) + " cycles, " +
             std::to_string(g.req_count) + " ports / " + std::to_string(g.leases) +
             " records, seed " + std::to_string(options.seed);
  } catch (const Failure& f) {
    reporter.Mismatch(f.what, "contract holds", "contract violated");
    passed = false;
    detail = "contract violated: " + f.what;
  }

  dut.final();
  reporter.Check(passed, "no contract violation in any phase");
  return reporter.Finish(passed ? "PASS" : "FAIL", detail);
}
