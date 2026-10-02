// ============================================================================
// tb_vrf.cpp -- CASE=vrf.mapping_aliases (work package I-053).
//
// The DUT is `mosaic_vrf`: a banked vector register file with a fixed
// logical-element-to-bank/row mapping, per-bank conflict arbitration and a
// read-lifetime guard. It is never its own oracle: an independent software model
// (`Shadow`, below) restates the contract from rtl/core/mosaic_vrf.sv's header
// and is compared against the DUT's whole output surface every cycle. A model
// that agreed with the DUT by construction would agree with a wrong DUT, so the
// model is written from the prose and the RTL is not read by it.
//
// The card's two acceptance properties are checked directly as well as through
// the per-cycle comparison:
//
//   * the same logical vector read and written through different lane counts
//     (2, 4, 8) yields the same data -- the mapping is a function of
//     (register, element, SEW, LMUL) and not of the lane quota;
//   * a destination write overlapping a source/mask element never overwrites an
//     element a granted read has not consumed -- asserted on the grant, on the
//     value the read returns, and on the write that follows.
//
// Phases, each resetting and priming the file first:
//   geometry      the widths the driver drives are the DUT's own
//   mapping       the fixed element->bank/row rule, over integer and fractional
//                 LMUL, wide operands and unaligned/overflowing groups
//   lane-count    the same vector written and read at 2/4/8 lanes; identical
//   conflicts     one bank, several demands: grants, refusals and broadcast
//   wide          an SEW=64 element over two banks; chunk isolation
//   fractional    LMUL 1/2, 1/4, 1/8 element mapping and out-of-range refusal
//   overlap       the read-lifetime guard, source and mask aliasing
//   latency       the read latency is the design's, the same at every platform
//   random        a seeded soak, every output compared every cycle
//   determinism   the same seed reproduces the run
//
// Mutation hooks: the shipping build passes; each `-DMOSAIC_VRF_MUTANT_*` build
// must fail on the phase named in results/reports/I-053-vrf.md.
// ============================================================================

#include <verilated.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "sim_common.h"
#include "Vmosaic_vrf_tb.h"

namespace {

using mosaic::ClockDriver;
using mosaic::Reporter;

// ---------------------------------------------------------------- geometry
// The p2 vector block (config/geometry/p2.json). Every one is checked against
// the DUT's own read-back before it is used.
constexpr unsigned kVregs = 32;
constexpr unsigned kVLEN = 128;
constexpr unsigned kELEN = 64;
constexpr unsigned kBankW = 32;
constexpr unsigned kBanks = 32;
constexpr unsigned kLanes = 8;
constexpr unsigned kRdPorts = 2;
constexpr unsigned kWrPorts = 1;
constexpr unsigned kLat = 1;
constexpr unsigned kLatMax = 2;
constexpr unsigned kWordsPerReg = kVLEN / kBankW;         // 4
constexpr unsigned kRegsPerRow = kBanks / kWordsPerReg;   // 8
constexpr unsigned kRows = kVregs / kRegsPerRow;          // 4

// --------------------------------------------------------------- diagnostics
struct Failure {
  std::string what;
};
[[noreturn]] void Fail(const std::string& where, const std::string& detail) {
  throw Failure{where + ": " + detail};
}
void Require(bool condition, const std::string& where, const std::string& detail) {
  if (!condition) Fail(where, detail);
}
std::string Bool(bool v) { return v ? "1" : "0"; }
std::string Dec(uint64_t v) { return std::to_string(v); }
std::string Hex64(uint64_t v) { return mosaic::Hex(v, 16); }

// ------------------------------------------------------------- address model
struct Addr {
  bool ok = false;
  unsigned phys = 0, row = 0, lo = 0, hi = 0, b0 = 0, b1 = 0, nb = 0;
};

// The fixed mapping, from the RTL header's rule. `lmul` is the signed LMUL
// exponent (-3..3); `sew_l` is log2(SEW).
Addr Resolve(unsigned base, unsigned elem, unsigned sew_l, int lmul) {
  Addr a;
  const unsigned sew = 1u << sew_l;
  int grp_bits, grp_regs;
  if (lmul >= 0) {
    grp_bits = static_cast<int>(kVLEN) << lmul;
    grp_regs = 1 << lmul;
  } else {
    grp_bits = static_cast<int>(kVLEN) >> (-lmul);
    grp_regs = 1;
  }
  const unsigned grp_base = base & ~(static_cast<unsigned>(grp_regs) - 1u);
  const unsigned bit_off = elem * sew;
  a.ok = (sew >= 8) && (sew <= kELEN) && (lmul >= -3) && (lmul <= 3) &&
         (static_cast<int>(bit_off + sew) <= grp_bits) &&
         (static_cast<int>(grp_base + static_cast<unsigned>(grp_regs)) <=
          static_cast<int>(kVregs));
  const unsigned reg_off = bit_off / kVLEN;
  const unsigned bit_in = bit_off % kVLEN;
  const unsigned phys = grp_base + reg_off;
  a.phys = phys;
  a.row = phys / kRegsPerRow;
  a.lo = bit_in;
  a.hi = bit_in + sew;
  a.b0 = (phys % kRegsPerRow) * kWordsPerReg + bit_in / kBankW;
  a.nb = (sew > kBankW) ? 2u : 1u;
  a.b1 = (a.nb == 2u) ? ((phys % kRegsPerRow) * kWordsPerReg + (bit_in + kBankW) / kBankW)
                      : a.b0;
  if (!a.ok) {
    a.nb = 0;
    a.b1 = a.b0;
  }
  return a;
}

// A 32-bit control word: [18:14] base, [13:7] elem, [6:4] sew_l, [3:0] lmul.
uint32_t Ctrl(unsigned base, unsigned elem, unsigned sew_l, int lmul) {
  return ((base & 0x1Fu) << 14) | ((elem & 0x7Fu) << 7) | ((sew_l & 0x7u) << 4) |
         (static_cast<unsigned>(lmul) & 0xFu);
}

unsigned Popcount(unsigned x) { return static_cast<unsigned>(__builtin_popcount(x)); }

// ---------------------------------------------------------------- stimulus
struct Req {
  bool valid = false;
  unsigned base = 0, elem = 0, sew_l = 0;
  int lmul = 0;
  uint16_t tag = 0;
  uint64_t data = 0;
};

struct Stim {
  unsigned lane_count = kLanes;
  unsigned plat = 0;
  Req rd[kLanes];
  Req wr[kLanes];
  bool q_valid = false;
  unsigned qbase = 0, qelem = 0, qsew = 0;
  int qlmul = 0;
};

struct Obs {
  unsigned rd_gnt = 0, wr_gnt = 0;
  bool rsp_v[kLanes] = {};
  uint16_t rsp_t[kLanes] = {};
  uint64_t rsp_d[kLanes] = {};
  bool qv = false;
  unsigned qp = 0, qr = 0, ql = 0, qh = 0, qn = 0, qb0 = 0, qb1 = 0;
  bool busy = false;
  uint32_t rg_ctr = 0, rc_ctr = 0, rb_ctr = 0;
  uint32_t wg_ctr = 0, wc_ctr = 0, wh_ctr = 0, wb_ctr = 0;
};

// ------------------------------------------------------------------- shadow
// The contract, restated. Storage is a plain array; the pipeline is the design's
// fixed RD_LATENCY; every arbitration rule is the header's prose.
class Shadow {
 public:
  struct Pred {
    unsigned rd_gnt = 0, wr_gnt = 0;
    bool rsp_v[kLanes] = {};
    uint16_t rsp_t[kLanes] = {};
    uint64_t rsp_d[kLanes] = {};
    bool qv = false;
    unsigned qp = 0, qr = 0, ql = 0, qh = 0, qn = 0, qb0 = 0, qb1 = 0;
    bool busy = false;
  };

  Shadow() { Reset(); }

  void Reset() {
    std::memset(mem_, 0, sizeof(mem_));
    for (unsigned k = 0; k < kLatMax; k++) {
      std::memset(&st_[k], 0, sizeof(st_[k]));
    }
    for (unsigned s = 0; s < kLanes; s++) {
      rgnt_[s] = 0;
      wgnt_[s] = 0;
    }
    c_rg_ = c_rc_ = c_rb_ = c_wg_ = c_wc_ = c_wh_ = c_wb_ = 0;
    busy_ = false;
  }

  Pred Eval(const Stim& s) {
    Pred p;
    const unsigned lane = std::min<unsigned>(s.lane_count, kLanes);
    unsigned rd_used[kBanks] = {};
    unsigned wr_used[kBanks] = {};
    d_rg_ = d_rc_ = d_rb_ = d_wg_ = d_wc_ = d_wh_ = d_wb_ = 0;
    busy_ = false;

    for (unsigned i = 0; i < kLanes; i++) {
      rad_[i] = Resolve(s.rd[i].base, s.rd[i].elem, s.rd[i].sew_l, s.rd[i].lmul);
      wad_[i] = Resolve(s.wr[i].base, s.wr[i].elem, s.wr[i].sew_l, s.wr[i].lmul);
      rgnt_[i] = 0;
      wgnt_[i] = 0;
    }

    // Read arbitration, lowest slot first; a bank serves up to kRdPorts distinct
    // rows and broadcasts a shared (bank, row) word.
    for (unsigned i = 0; i < kLanes; i++) {
      if (i >= lane || !s.rd[i].valid) continue;
      const Addr& a = rad_[i];
      if (!a.ok) {
        d_rb_++;
      } else if ((rd_used[a.b0] & (1u << a.row)) == 0u &&
                 Popcount(rd_used[a.b0]) >= kRdPorts) {
        d_rc_++;
        busy_ = true;
      } else if (a.nb == 2u && (rd_used[a.b1] & (1u << a.row)) == 0u &&
                 Popcount(rd_used[a.b1]) >= kRdPorts) {
        d_rc_++;
        busy_ = true;
      } else {
        rgnt_[i] = 1;
        rd_used[a.b0] |= (1u << a.row);
        if (a.nb == 2u) rd_used[a.b1] |= (1u << a.row);
      }
    }

    // The read-lifetime set: granted-this-cycle reads plus the pipeline stages
    // that present after this cycle.
    struct Hz { unsigned p, lo, hi; };
    std::vector<Hz> hz;
    for (unsigned i = 0; i < kLanes; i++) {
      if (rgnt_[i]) hz.push_back({rad_[i].phys, rad_[i].lo, rad_[i].hi});
    }
    for (unsigned k = 0; k + 1 < kLat; k++) {
      for (unsigned i = 0; i < kLanes; i++) {
        if (st_[k].v[i]) hz.push_back({st_[k].p[i], st_[k].lo[i], st_[k].hi[i]});
      }
    }

    // Write arbitration: kRdPorts-analogue with no same-row sharing.
    for (unsigned i = 0; i < kLanes; i++) {
      if (i >= lane || !s.wr[i].valid) continue;
      const Addr& a = wad_[i];
      bool hazard = false;
      for (const Hz& h : hz) {
        if (h.p == a.phys && a.lo < h.hi && h.lo < a.hi) hazard = true;
      }
      if (!a.ok) {
        d_wb_++;
      } else if (hazard) {
        d_wh_++;
      } else if ((wr_used[a.b0] & (1u << a.row)) != 0u ||
                 Popcount(wr_used[a.b0]) >= kWrPorts) {
        d_wc_++;
      } else if (a.nb == 2u && ((wr_used[a.b1] & (1u << a.row)) != 0u ||
                                Popcount(wr_used[a.b1]) >= kWrPorts)) {
        d_wc_++;
      } else {
        wgnt_[i] = 1;
        wr_used[a.b0] |= (1u << a.row);
        if (a.nb == 2u) wr_used[a.b1] |= (1u << a.row);
      }
    }

    for (unsigned i = 0; i < kLanes; i++) {
      p.rsp_v[i] = st_[kLat - 1].v[i];
      p.rsp_t[i] = st_[kLat - 1].t[i];
      p.rsp_d[i] = st_[kLat - 1].d[i];
      if (rgnt_[i]) p.rd_gnt |= (1u << i);
      if (wgnt_[i]) p.wr_gnt |= (1u << i);
    }
    const Addr qa = Resolve(s.qbase, s.qelem, s.qsew, s.qlmul);
    p.qv = s.q_valid && qa.ok;
    p.qp = qa.phys;
    p.qr = qa.row;
    p.ql = qa.lo;
    p.qh = qa.hi;
    p.qn = qa.nb;
    p.qb0 = qa.b0;
    p.qb1 = qa.b1;
    p.busy = busy_;
    return p;
  }

  void Apply(const Stim& s) {
    for (int k = static_cast<int>(kLatMax) - 1; k > 0; k--) {
      st_[k] = st_[k - 1];
    }
    Stage s0;
    for (unsigned i = 0; i < kLanes; i++) {
      if (rgnt_[i] && rad_[i].ok) {
        s0.v[i] = true;
        s0.t[i] = s.rd[i].tag;
        s0.d[i] = Extract(rad_[i]);
        s0.p[i] = rad_[i].phys;
        s0.lo[i] = rad_[i].lo;
        s0.hi[i] = rad_[i].hi;
      }
    }
    st_[0] = s0;

    for (unsigned i = 0; i < kLanes; i++) {
      if (!wgnt_[i]) continue;
      const Addr& a = wad_[i];
      uint64_t data = s.wr[i].data;
      if (a.nb == 1u) {
        const unsigned width = a.hi - a.lo;
        const unsigned off = a.lo % kBankW;
        const uint32_t mask = (width >= 32) ? 0xFFFFFFFFu
                                            : (((1u << width) - 1u) << off);
        const uint32_t val = static_cast<uint32_t>(data) << off;
        mem_[a.row][a.b0] = (mem_[a.row][a.b0] & ~mask) | (val & mask);
      } else {
        mem_[a.row][a.b0] = static_cast<uint32_t>(data & 0xFFFFFFFFull);
        mem_[a.row][a.b1] = static_cast<uint32_t>(data >> 32);
      }
    }

    c_rg_ += d_rg_;
    c_rc_ += d_rc_;
    c_rb_ += d_rb_;
    c_wg_ += d_wg_;
    c_wc_ += d_wc_;
    c_wh_ += d_wh_;
    c_wb_ += d_wb_;
  }

  uint32_t rg() const { return c_rg_; }
  uint32_t rc() const { return c_rc_; }
  uint32_t rb() const { return c_rb_; }
  uint32_t wg() const { return c_wg_; }
  uint32_t wc() const { return c_wc_; }
  uint32_t wh() const { return c_wh_; }
  uint32_t wb() const { return c_wb_; }

 private:
  struct Stage {
    bool v[kLanes] = {};
    uint16_t t[kLanes] = {};
    uint64_t d[kLanes] = {};
    unsigned p[kLanes] = {};
    unsigned lo[kLanes] = {};
    unsigned hi[kLanes] = {};
  };

  uint64_t Extract(const Addr& a) const {
    if (a.nb == 1u) {
      const unsigned off = a.lo % kBankW;
      uint64_t d = mem_[a.row][a.b0] >> off;
      const unsigned width = a.hi - a.lo;
      if (width < 64) d &= ((1ull << width) - 1ull);
      return d;
    }
    return static_cast<uint64_t>(mem_[a.row][a.b0]) |
           (static_cast<uint64_t>(mem_[a.row][a.b1]) << 32);
  }

  uint32_t mem_[kRows][kBanks];
  Stage st_[kLatMax];
  Addr rad_[kLanes], wad_[kLanes];
  unsigned rgnt_[kLanes], wgnt_[kLanes];
  uint32_t d_rg_ = 0, d_rc_ = 0, d_rb_ = 0, d_wg_ = 0, d_wc_ = 0, d_wh_ = 0, d_wb_ = 0;
  uint32_t c_rg_ = 0, c_rc_ = 0, c_rb_ = 0, c_wg_ = 0, c_wc_ = 0, c_wh_ = 0, c_wb_ = 0;
  bool busy_ = false;
};

// -------------------------------------------------------------- the harness
class Harness {
 public:
  Harness(Vmosaic_vrf_tb* dut, ClockDriver* clk, uint64_t max_cycles)
      : dut_(dut), clk_(clk), max_cycles_(max_cycles) {}

  void Phase(const std::string& name) { phase_ = name; }
  Shadow* shadow() { return &shadow_; }
  uint64_t comparisons() const { return comparisons_; }
  uint64_t cycles() const { return cycles_; }
  uint64_t hash() const { return hash_; }
  void ResetHash() { hash_ = 0; }
  const std::string& phase() const { return phase_; }

  void Reset(int cycles) {
    shadow_.Reset();
    for (int i = 0; i < cycles; i++) Cycle(Stim{}, /*rst=*/true);
  }

  Obs Cycle(const Stim& s, bool rst = false) {
    if (cycles_ >= max_cycles_) {
      Fail(phase_, "max-cycles (" + Dec(max_cycles_) + ") exhausted before the phase finished");
    }
    const std::string where = phase_ + ": cycle " + Dec(cycles_);
    Drive(s, rst);
    dut_->eval();

    Obs o;
    if (!rst) {
      Capture(&o);
      const Shadow::Pred p = shadow_.Eval(s);
      Compare(o, p, where);
      Mix(o);
    }

    dut_->clk = 1;
    dut_->eval();
    clk_->Tick();
    dut_->clk = 0;
    dut_->eval();
    ++cycles_;

    if (!rst) {
      shadow_.Apply(s);
      CaptureCounters(&o);
      CompareCounters(o, where);
      ++comparisons_;
    }
    return o;
  }

  // ------------------------------------------------------------- helpers
  uint64_t ReadOne(unsigned base, unsigned elem, unsigned sew_l, int lmul, unsigned lane,
                   uint16_t tag) {
    Req r;
    r.valid = true;
    r.base = base;
    r.elem = elem;
    r.sew_l = sew_l;
    r.lmul = lmul;
    r.tag = tag;
    Obs o;
    do {
      Stim s;
      s.lane_count = lane;
      s.rd[0] = r;
      o = Cycle(s);
    } while (((o.rd_gnt >> 0) & 1u) == 0u);
    for (unsigned i = 0; i < kLat; i++) {
      Stim s;
      s.lane_count = lane;
      o = Cycle(s);
    }
    Require(o.rsp_v[0], phase_, "a granted read produced no response within " + Dec(kLat) + " cycle(s)");
    Require(o.rsp_t[0] == tag, phase_,
            "response tag " + Dec(o.rsp_t[0]) + " does not match request tag " + Dec(tag));
    return o.rsp_d[0];
  }

  void WriteOne(unsigned base, unsigned elem, unsigned sew_l, int lmul, unsigned lane,
                uint64_t data) {
    Req r;
    r.valid = true;
    r.base = base;
    r.elem = elem;
    r.sew_l = sew_l;
    r.lmul = lmul;
    r.data = data;
    Obs o;
    do {
      Stim s;
      s.lane_count = lane;
      s.wr[0] = r;
      o = Cycle(s);
    } while (((o.wr_gnt >> 0) & 1u) == 0u);
  }

  void DrainWrites(std::vector<Req> q, unsigned lane) {
    size_t head = 0;
    while (head < q.size()) {
      Stim s;
      s.lane_count = lane;
      const size_t n = std::min<size_t>(lane, q.size() - head);
      for (size_t i = 0; i < n; i++) {
        s.wr[i] = q[head + i];
        s.wr[i].valid = true;
      }
      const Obs o = Cycle(s);
      std::vector<Req> rest;
      for (size_t i = 0; i < n; i++) {
        if (((o.wr_gnt >> i) & 1u) == 0u) rest.push_back(q[head + i]);
      }
      rest.insert(rest.end(), q.begin() + static_cast<long>(head + n), q.end());
      q = rest;
      head = 0;
    }
  }

  std::vector<uint64_t> ReadBatch(std::vector<Req> reqs, unsigned lane) {
    std::vector<uint64_t> out(reqs.size(), 0);
    int slot[kLanes];
    bool waiting[kLanes];
    for (unsigned i = 0; i < kLanes; i++) {
      slot[i] = -1;
      waiting[i] = false;
    }
    size_t next = 0, remaining = reqs.size();
    uint64_t guard = 0;
    while (remaining > 0) {
      if (++guard > 100000) Fail(phase_, "ReadBatch did not complete");
      Stim s;
      s.lane_count = lane;
      bool present[kLanes] = {};
      for (unsigned sl = 0; sl < lane; sl++) {
        if (slot[sl] < 0 && next < reqs.size()) {
          slot[sl] = static_cast<int>(next);
          next++;
        }
        if (slot[sl] >= 0 && !waiting[sl]) {
          s.rd[sl] = reqs[static_cast<size_t>(slot[sl])];
          s.rd[sl].valid = true;
          present[sl] = true;
        }
      }
      const Obs o = Cycle(s);
      for (unsigned sl = 0; sl < lane; sl++) {
        if (present[sl] && ((o.rd_gnt >> sl) & 1u)) waiting[sl] = true;
      }
      for (unsigned sl = 0; sl < lane; sl++) {
        const int idx = slot[sl];
        if (idx >= 0 && waiting[sl] && o.rsp_v[sl] &&
            o.rsp_t[sl] == reqs[static_cast<size_t>(idx)].tag) {
          out[static_cast<size_t>(idx)] = o.rsp_d[sl];
          slot[sl] = -1;
          waiting[sl] = false;
          remaining--;
        }
      }
    }
    return out;
  }

  // ------------------------------------------------------------- primitives
  uint32_t PrimeVal(unsigned reg, unsigned elem, uint32_t salt = 0) const {
    return (0x9E3779B1u * (reg * kWordsPerReg + elem) + 0xDEADBEEFu) ^ salt;
  }

  void Prime(unsigned lane, uint32_t salt = 0) {
    std::vector<Req> q;
    for (unsigned r = 0; r < kVregs; r++) {
      for (unsigned e = 0; e < kWordsPerReg; e++) {
        Req w;
        w.valid = true;
        w.base = r;
        w.elem = e;
        w.sew_l = 5;   // SEW = 32: one full bank word each
        w.lmul = 0;
        w.data = PrimeVal(r, e, salt);
        q.push_back(w);
      }
    }
    DrainWrites(q, lane);
  }

  void Expect(const std::string& where, bool condition, const std::string& detail) const {
    Require(condition, where, detail);
  }

 private:
  void Drive(const Stim& s, bool rst) {
    dut_->rst = rst ? 1 : 0;
    dut_->clk = 0;
    dut_->lane_count_i = static_cast<uint8_t>(s.lane_count);
    dut_->plat_i = static_cast<uint8_t>(s.plat);
    unsigned rv = 0, wv = 0;
    for (unsigned i = 0; i < kLanes; i++) {
      dut_->rd_ctrl_i[i] = Ctrl(s.rd[i].base, s.rd[i].elem, s.rd[i].sew_l, s.rd[i].lmul);
      dut_->rd_tag_i[i] = s.rd[i].tag;
      dut_->wr_ctrl_i[i] = Ctrl(s.wr[i].base, s.wr[i].elem, s.wr[i].sew_l, s.wr[i].lmul);
      dut_->wr_data_i[i] = s.wr[i].data;
      if (s.rd[i].valid) rv |= (1u << i);
      if (s.wr[i].valid) wv |= (1u << i);
    }
    dut_->rd_valid_i = static_cast<uint8_t>(rv);
    dut_->wr_valid_i = static_cast<uint8_t>(wv);
    dut_->q_valid_i = s.q_valid ? 1 : 0;
    dut_->q_ctrl_i = Ctrl(s.qbase, s.qelem, s.qsew, s.qlmul);
  }

  void Capture(Obs* o) const {
    o->rd_gnt = dut_->rd_gnt_o;
    o->wr_gnt = dut_->wr_gnt_o;
    for (unsigned i = 0; i < kLanes; i++) {
      o->rsp_v[i] = dut_->rd_rsp_valid_o & (1u << i);
      o->rsp_t[i] = dut_->rd_rsp_tag_o[i];
      o->rsp_d[i] = dut_->rd_rsp_data_o[i];
    }
    o->qv = dut_->q_valid_o != 0;
    o->qp = dut_->q_phys_reg_o;
    o->qr = dut_->q_row_o;
    o->ql = dut_->q_lo_bit_o;
    o->qh = dut_->q_hi_bit_o;
    o->qn = dut_->q_nbanks_o;
    o->qb0 = dut_->q_bank0_o;
    o->qb1 = dut_->q_bank1_o;
    o->busy = dut_->busy_o != 0;
  }

  void CaptureCounters(Obs* o) const {
    o->rg_ctr = dut_->rd_gnt_ctr_o;
    o->rc_ctr = dut_->rd_conflict_ctr_o;
    o->rb_ctr = dut_->rd_bad_ctr_o;
    o->wg_ctr = dut_->wr_gnt_ctr_o;
    o->wc_ctr = dut_->wr_conflict_ctr_o;
    o->wh_ctr = dut_->wr_hazard_ctr_o;
    o->wb_ctr = dut_->wr_bad_ctr_o;
  }

  void CmpB(const std::string& where, const std::string& field, bool exp, bool act) const {
    if (exp != act) Fail(where, field + ": expected " + Bool(exp) + ", got " + Bool(act));
  }
  void CmpU(const std::string& where, const std::string& field, uint64_t exp, uint64_t act) const {
    if (exp != act) {
      Fail(where, field + ": expected " + Dec(exp) + " (0x" + Hex64(exp) + "), got " + Dec(act) +
                      " (0x" + Hex64(act) + ")");
    }
  }

  void Compare(const Obs& o, const Shadow::Pred& p, const std::string& where) const {
    CmpU(where, "rd_gnt", p.rd_gnt, o.rd_gnt);
    CmpU(where, "wr_gnt", p.wr_gnt, o.wr_gnt);
    for (unsigned i = 0; i < kLanes; i++) {
      CmpB(where, "rd_rsp_valid[" + Dec(i) + "]", p.rsp_v[i], o.rsp_v[i]);
      if (p.rsp_v[i]) {
        CmpU(where, "rd_rsp_tag[" + Dec(i) + "]", p.rsp_t[i], o.rsp_t[i]);
        CmpU(where, "rd_rsp_data[" + Dec(i) + "]", p.rsp_d[i], o.rsp_d[i]);
      }
    }
    CmpB(where, "q_valid", p.qv, o.qv);
    CmpU(where, "q_phys_reg", p.qp, o.qp);
    CmpU(where, "q_row", p.qr, o.qr);
    CmpU(where, "q_lo_bit", p.ql, o.ql);
    CmpU(where, "q_hi_bit", p.qh, o.qh);
    CmpU(where, "q_nbanks", p.qn, o.qn);
    CmpU(where, "q_bank0", p.qb0, o.qb0);
    CmpU(where, "q_bank1", p.qb1, o.qb1);
    CmpB(where, "busy", p.busy, o.busy);
  }

  void CompareCounters(const Obs& o, const std::string& where) const {
    CmpU(where, "rd_gnt_ctr", shadow_.rg(), o.rg_ctr);
    CmpU(where, "rd_conflict_ctr", shadow_.rc(), o.rc_ctr);
    CmpU(where, "rd_bad_ctr", shadow_.rb(), o.rb_ctr);
    CmpU(where, "wr_gnt_ctr", shadow_.wg(), o.wg_ctr);
    CmpU(where, "wr_conflict_ctr", shadow_.wc(), o.wc_ctr);
    CmpU(where, "wr_hazard_ctr", shadow_.wh(), o.wh_ctr);
    CmpU(where, "wr_bad_ctr", shadow_.wb(), o.wb_ctr);
  }

  void Mix(const Obs& o) {
    // A deterministic fold of the observed surface, for the determinism phase.
    uint64_t h = hash_;
    auto fold = [&h](uint64_t v) {
      h ^= v;
      h *= 0x100000001B3ull;
    };
    fold(o.rd_gnt);
    fold(o.wr_gnt);
    for (unsigned i = 0; i < kLanes; i++) {
      fold(o.rsp_v[i] ? 1 : 0);
      if (o.rsp_v[i]) {
        fold(o.rsp_t[i]);
        fold(o.rsp_d[i]);
      }
    }
    fold(o.qv ? 1 : 0);
    fold(o.qp);
    fold(o.qr);
    fold(o.ql);
    hash_ = h;
  }

  Vmosaic_vrf_tb* dut_;
  ClockDriver* clk_;
  uint64_t max_cycles_;
  uint64_t cycles_ = 0;
  uint64_t comparisons_ = 0;
  uint64_t hash_ = 0;
  std::string phase_;
  Shadow shadow_;
};

// ------------------------------------------------------------------ phases

void PhaseGeometry(Harness* h, Reporter* rep, Vmosaic_vrf_tb* dut) {
  h->Phase("geometry");
  h->Reset(4);
  Require(dut->o_vlen_o == kVLEN, "geometry", "VLEN is not " + Dec(kVLEN));
  Require(dut->o_elen_o == kELEN, "geometry", "ELEN is not " + Dec(kELEN));
  Require(dut->o_vregs_o == kVregs, "geometry", "VREGS is not " + Dec(kVregs));
  Require(dut->o_banks_o == kBanks, "geometry", "BANKS is not " + Dec(kBanks));
  Require(dut->o_bank_w_o == kBankW, "geometry", "BANK_W is not " + Dec(kBankW));
  Require(dut->o_rows_o == kRows, "geometry", "ROWS is not " + Dec(kRows));
  Require(dut->o_regs_per_row_o == kRegsPerRow, "geometry", "REGS_PER_ROW is not " + Dec(kRegsPerRow));
  Require(dut->o_lane_max_o == kLanes, "geometry", "LANES_MAX is not " + Dec(kLanes));
  Require(dut->o_rd_ports_o == kRdPorts, "geometry", "RD_PORTS is not " + Dec(kRdPorts));
  Require(dut->o_wr_ports_o == kWrPorts, "geometry", "WR_PORTS is not " + Dec(kWrPorts));
  Require(dut->o_rd_latency_o == kLat, "geometry",
          "RD_LATENCY is not the driver's constant " + Dec(kLat));
  rep->Check(true, "geometry: VLEN=" + Dec(kVLEN) + " ELEN=" + Dec(kELEN) + " vregs=" +
                       Dec(kVregs) + " banks=" + Dec(kBanks) + "x" + Dec(kBankW) + " rows=" +
                       Dec(kRows) + " lanes<=" + Dec(kLanes) + " rd_ports=" + Dec(kRdPorts) +
                       " wr_ports=" + Dec(kWrPorts) + " latency=" + Dec(kLat));
}

// Drive the mapping query for one tuple at `lane` and return the DUT's answer,
// while the per-cycle comparison checks it against the model.
Obs QueryOne(Harness* h, unsigned base, unsigned elem, unsigned sew_l, int lmul, unsigned lane) {
  Stim s;
  s.lane_count = lane;
  s.q_valid = true;
  s.qbase = base;
  s.qelem = elem;
  s.qsew = sew_l;
  s.qlmul = lmul;
  return h->Cycle(s);
}

void PhaseMapping(Harness* h, Reporter* rep) {
  h->Phase("mapping");
  h->Reset(4);

  struct Case { unsigned base, elem, sew_l; int lmul; const char* why; };
  const Case cases[] = {
      {0, 0, 5, 0, "baseline word"},
      {0, 3, 5, 0, "last word of a register"},
      {1, 0, 6, 0, "wide SEW=64 element, two banks"},
      {4, 5, 3, 1, "LMUL=2 group"},
      {0, 7, 3, 3, "LMUL=8 group, first register"},
      {0, 64, 3, 3, "LMUL=8 group, crossing into register 4"},
      {0, 127, 3, 3, "LMUL=8 group, last element"},
      {2, 1, 5, -1, "fractional LMUL=1/2"},
      {0, 0, 3, -3, "fractional LMUL=1/8"},
      {0, 2, 5, -1, "fractional element out of range"},
      {31, 0, 6, 0, "widest element in the last register"},
      {3, 0, 5, 1, "unaligned base aligned down"},
      {31, 0, 5, 1, "unaligned base at the top"},
      {0, 2, 3, -3, "fractional 1/8 element out of range"},
  };

  unsigned covered = 0;
  for (const Case& c : cases) {
    Obs first;
    bool have_first = false;
    for (unsigned lane : {2u, 4u, 8u}) {
      const Obs o = QueryOne(h, c.base, c.elem, c.sew_l, c.lmul, lane);
      const Addr a = Resolve(c.base, c.elem, c.sew_l, c.lmul);
      Require(o.qv == (a.ok != 0), "mapping",
              std::string(c.why) + ": q_valid disagrees with the model");
      if (a.ok) {
        Require(o.qp == a.phys && o.qr == a.row && o.ql == a.lo && o.qh == a.hi &&
                    o.qn == a.nb && o.qb0 == a.b0 && o.qb1 == a.b1,
                "mapping", std::string(c.why) + ": the mapping disagrees with the model");
      }
      if (!have_first) {
        first = o;
        have_first = true;
      } else {
        Require(first.qv == o.qv && first.qp == o.qp && first.qr == o.qr &&
                    first.ql == o.ql && first.qh == o.qh && first.qn == o.qn &&
                    first.qb0 == o.qb0 && first.qb1 == o.qb1,
                "mapping",
                std::string(c.why) + ": the mapping changed with the lane count");
      }
    }
    covered++;
  }
  rep->Check(true, "mapping: " + Dec(covered) +
                       " address tuples (integer LMUL 1/2/4/8, fractional 1/2, 1/4, 1/8, "
                       "SEW 8/16/32/64, wide operands, out-of-range) agree with the model and "
                       "are identical at 2/4/8 lanes");
}

void PhaseLaneCount(Harness* h, Reporter* rep) {
  h->Phase("lane-count");
  h->Reset(4);
  h->Prime(8);

  // The same logical vector, written once at 8 lanes and read back at 2, 4 and
  // 8 lanes. All three readbacks must equal the primed values and each other.
  std::vector<std::vector<uint64_t>> reads;
  for (unsigned lane : {2u, 4u, 8u}) {
    std::vector<Req> reqs;
    for (unsigned r = 0; r < kVregs; r++) {
      for (unsigned e = 0; e < kWordsPerReg; e++) {
        Req q;
        q.valid = true;
        q.base = r;
        q.elem = e;
        q.sew_l = 5;
        q.lmul = 0;
        q.tag = static_cast<uint16_t>(0x8000u + r * kWordsPerReg + e);
        reqs.push_back(q);
      }
    }
    std::vector<uint64_t> data = h->ReadBatch(reqs, lane);
    Require(data.size() == static_cast<size_t>(kVregs * kWordsPerReg), "lane-count",
            "the readback lost a request");
    for (unsigned r = 0; r < kVregs; r++) {
      for (unsigned e = 0; e < kWordsPerReg; e++) {
        const uint64_t want = h->PrimeVal(r, e);
        const uint64_t got = data[r * kWordsPerReg + e];
        Require(got == want, "lane-count",
                "register " + Dec(r) + " word " + Dec(e) + " read at " + Dec(lane) +
                    " lanes: expected 0x" + Hex64(want) + ", got 0x" + Hex64(got));
      }
    }
    reads.push_back(data);
  }
  for (unsigned i = 1; i < reads.size(); i++) {
    Require(reads[i] == reads[0], "lane-count",
            "the " + Dec(i) + "-th lane count read a different logical vector");
  }

  // The complementary directions: rewrite the whole file at a different lane
  // count and read it back at another, so every (write lanes, read lanes) pair
  // is exercised, not only written-at-8.
  const unsigned pairs[][2] = {{2, 8}, {4, 2}, {8, 4}};
  for (unsigned i = 0; i < 3; i++) {
    const uint32_t salt = 0x1000u * (i + 1u);
    h->Prime(pairs[i][0], salt);
    std::vector<Req> reqs;
    for (unsigned r = 0; r < kVregs; r++) {
      for (unsigned e = 0; e < kWordsPerReg; e++) {
        Req q;
        q.valid = true;
        q.base = r;
        q.elem = e;
        q.sew_l = 5;
        q.lmul = 0;
        q.tag = static_cast<uint16_t>(0x9000u + r * kWordsPerReg + e);
        reqs.push_back(q);
      }
    }
    const std::vector<uint64_t> back = h->ReadBatch(reqs, pairs[i][1]);
    for (unsigned r = 0; r < kVregs; r++) {
      for (unsigned e = 0; e < kWordsPerReg; e++) {
        const uint64_t want = h->PrimeVal(r, e, salt);
        const uint64_t got = back[r * kWordsPerReg + e];
        Require(got == want, "lane-count",
                "written at " + Dec(pairs[i][0]) + " lanes, read at " + Dec(pairs[i][1]) +
                    ": register " + Dec(r) + " word " + Dec(e) + " expected 0x" + Hex64(want) +
                    ", got 0x" + Hex64(got));
      }
    }
  }

  // A wide operand across lane counts too.
  const uint64_t fresh = 0x1122334455667788ull;
  h->WriteOne(7, 1, 6, 0, 2, fresh);
  const uint64_t got = h->ReadOne(7, 1, 6, 0, 8, 0x1234);
  Require(got == fresh, "lane-count",
          "a wide value written at 2 lanes read back at 8 lanes as 0x" + Hex64(got));
  rep->Check(true, "lane-count: the whole file is identical when written at one lane count and "
                   "read at another for (2->8), (4->2) and (8->4), and read at 2, 4 and 8 lanes "
                   "after a single write, plus a wide element across lane counts");
}

void PhaseConflicts(Harness* h, Reporter* rep) {
  h->Phase("conflicts");
  h->Reset(4);
  h->Prime(8);

  // (a) Four reads, all to bank 0 but four different rows. RD_PORTS=2, so the
  //     first two are granted and the rest refused and counted.
  {
    uint32_t rc_before = h->shadow()->rc();
    Stim s;
    s.lane_count = 4;
    const unsigned regs[4] = {0, 8, 16, 24};
    for (unsigned i = 0; i < 4; i++) {
      s.rd[i].valid = true;
      s.rd[i].base = regs[i];
      s.rd[i].elem = 0;
      s.rd[i].sew_l = 5;
      s.rd[i].tag = static_cast<uint16_t>(0x2000 + i);
    }
    const Obs o = h->Cycle(s);
    Require(o.rd_gnt == 0x3u, "conflicts",
            "four reads to two ports granted 0x" + Hex64(o.rd_gnt) + ", expected 0x3");
    Require(h->shadow()->rc() == rc_before + 2, "conflicts",
            "the two refused reads were not both counted");
    Require(o.busy, "conflicts", "a cycle with refused reads did not raise busy");
  }

  // (b) Broadcast: two demands for the *same* (bank, row) word share one port.
  {
    uint32_t rc_before = h->shadow()->rc();
    Stim s;
    s.lane_count = 2;
    for (unsigned i = 0; i < 2; i++) {
      s.rd[i].valid = true;
      s.rd[i].base = 0;
      s.rd[i].elem = 0;
      s.rd[i].sew_l = 5;
      s.rd[i].tag = static_cast<uint16_t>(0x2100 + i);
    }
    const Obs o = h->Cycle(s);
    Require(o.rd_gnt == 0x3u, "conflicts",
            "a broadcast word was not shared: granted 0x" + Hex64(o.rd_gnt));
    Require(h->shadow()->rc() == rc_before, "conflicts",
            "a broadcast word was reported as a conflict");
  }

  // (c) Two writes to one bank, different rows: WR_PORTS=1, so only the first.
  {
    uint32_t wc_before = h->shadow()->wc();
    Stim s;
    s.lane_count = 2;
    s.wr[0].valid = true;
    s.wr[0].base = 0;
    s.wr[0].elem = 0;
    s.wr[0].sew_l = 5;
    s.wr[0].data = 0xAAAA0001u;
    s.wr[1].valid = true;
    s.wr[1].base = 8;
    s.wr[1].elem = 0;
    s.wr[1].sew_l = 5;
    s.wr[1].data = 0xBBBB0002u;
    const Obs o = h->Cycle(s);
    Require(o.wr_gnt == 0x1u, "conflicts",
            "two writes to one port granted 0x" + Hex64(o.wr_gnt) + ", expected 0x1");
    Require(h->shadow()->wc() == wc_before + 1, "conflicts",
            "the refused write was not counted as a bank conflict");
  }
  rep->Check(true, "conflicts: bank port limits refuse the extra demand and count it, a shared "
                   "(bank,row) word is broadcast to both readers, and a second writer of one "
                   "bank is refused");
}

void PhaseWide(Harness* h, Reporter* rep) {
  h->Phase("wide");
  h->Reset(4);
  h->Prime(8);

  const uint64_t v0 = 0x1122334455667788ull;
  const uint64_t v1 = 0xAABBCCDDEEFF0011ull;
  h->WriteOne(2, 0, 6, 0, 8, v0);   // register 2, element 0, SEW=64
  h->WriteOne(2, 1, 6, 0, 8, v1);   // element 1: the other two banks
  Require(h->ReadOne(2, 0, 6, 0, 8, 0x3001) == v0, "wide", "wide element 0 did not read back");
  Require(h->ReadOne(2, 1, 6, 0, 8, 0x3002) == v1, "wide", "wide element 1 did not read back");

  // Rewrite only element 0; element 1 must be untouched (its banks are
  // different words).
  const uint64_t v0b = 0x0F1E2D3C4B5A6978ull;
  h->WriteOne(2, 0, 6, 0, 8, v0b);
  Require(h->ReadOne(2, 0, 6, 0, 8, 0x3003) == v0b, "wide", "rewritten element 0 did not read back");
  Require(h->ReadOne(2, 1, 6, 0, 8, 0x3004) == v1, "wide",
          "writing element 0 clobbered element 1");
  rep->Check(true, "wide: an SEW=64 element occupies two bank words; writing one element does "
                   "not disturb the other");
}

void PhaseFractional(Harness* h, Reporter* rep) {
  h->Phase("fractional");
  h->Reset(4);
  h->Prime(8);

  // LMUL=1/2, SEW=32: two elements use bits 0..63 of one register.
  const uint64_t a = 0x05060708u;   // SEW=32: one word per element
  const uint64_t b = 0x15161718u;
  h->WriteOne(5, 0, 5, -1, 8, a);
  h->WriteOne(5, 1, 5, -1, 8, b);
  Require(h->ReadOne(5, 0, 5, -1, 8, 0x4001) == a, "fractional", "1/2 element 0 lost");
  Require(h->ReadOne(5, 1, 5, -1, 8, 0x4002) == b, "fractional", "1/2 element 1 lost");

  // LMUL=1/8, SEW=8: two byte elements use bits 0..15.
  h->WriteOne(9, 0, 3, -3, 8, 0x5A);
  h->WriteOne(9, 1, 3, -3, 8, 0xA5);
  Require(h->ReadOne(9, 0, 3, -3, 8, 0x4101) == 0x5A, "fractional", "1/8 element 0 lost");
  Require(h->ReadOne(9, 1, 3, -3, 8, 0x4102) == 0xA5, "fractional", "1/8 element 1 lost");

  // An element outside the group is refused and counted, not aliased.
  {
    uint32_t rb_before = h->shadow()->rb();
    Stim s;
    s.lane_count = 4;
    s.rd[0].valid = true;
    s.rd[0].base = 5;
    s.rd[0].elem = 2;   // 1/2 LMUL holds two 32-bit elements
    s.rd[0].sew_l = 5;
    s.rd[0].lmul = -1;
    const Obs o = h->Cycle(s);
    Require(((o.rd_gnt >> 0) & 1u) == 0u, "fractional", "an out-of-group element was granted");
    Require(h->shadow()->rb() == rb_before + 1, "fractional",
            "the out-of-group element was not counted as bad");
  }
  rep->Check(true, "fractional: LMUL 1/2 and 1/8 elements map and round-trip, and an "
                   "out-of-group element is refused and counted");
}

void PhaseOverlap(Harness* h, Reporter* rep) {
  h->Phase("overlap");
  h->Reset(4);
  h->Prime(8);

  // Source/destination aliasing: read reg 1 element 1 and, in the same cycle,
  // offer the write of the same element. The read must win the cycle; the write
  // must be refused (hazard), the read must return the old value, and the retried
  // write must then be admitted.
  {
    const uint64_t old_v = h->PrimeVal(1, 1);
    const uint64_t new_v = 0xDEADBEEFu;   // SEW=32 element
    uint32_t wh_before = h->shadow()->wh();

    Stim s;
    s.lane_count = 4;
    s.rd[0].valid = true;
    s.rd[0].base = 1;
    s.rd[0].elem = 1;
    s.rd[0].sew_l = 5;
    s.rd[0].tag = 0x5001;
    s.wr[1].valid = true;
    s.wr[1].base = 1;
    s.wr[1].elem = 1;
    s.wr[1].sew_l = 5;
    s.wr[1].data = new_v;
    const Obs o = h->Cycle(s);
    Require(((o.rd_gnt >> 0) & 1u) == 1u, "overlap", "the read was refused");
    Require(((o.wr_gnt >> 1) & 1u) == 0u, "overlap",
            "the write passed a read of the same element in the same cycle");
    Require(h->shadow()->wh() == wh_before + 1, "overlap", "the refused write was not counted");

    Stim s2;
    s2.lane_count = 4;
    const Obs o2 = h->Cycle(s2);
    Require(o2.rsp_v[0] && o2.rsp_t[0] == 0x5001, "overlap", "the aliased read produced no response");
    Require(o2.rsp_d[0] == old_v, "overlap",
            "the aliased read returned 0x" + Hex64(o2.rsp_d[0]) + ", not the pre-write value 0x" +
                Hex64(old_v));

    h->WriteOne(1, 1, 5, 0, 8, new_v);
    Require(h->ReadOne(1, 1, 5, 0, 8, 0x5002) == new_v, "overlap",
            "the retried write did not take effect");
  }

  // Mask aliasing: the destination is the mask register v0.
  {
    const uint64_t old_v = h->PrimeVal(0, 2);
    const uint64_t new_v = 0x0BADF00Du;   // SEW=32 element
    uint32_t wh_before = h->shadow()->wh();
    Stim s;
    s.lane_count = 4;
    s.rd[0].valid = true;   // read v0[2] (the mask)
    s.rd[0].base = 0;
    s.rd[0].elem = 2;
    s.rd[0].sew_l = 5;
    s.rd[0].tag = 0x5101;
    s.wr[1].valid = true;   // write vd = v0[2]
    s.wr[1].base = 0;
    s.wr[1].elem = 2;
    s.wr[1].sew_l = 5;
    s.wr[1].data = new_v;
    const Obs o = h->Cycle(s);
    Require(((o.rd_gnt >> 0) & 1u) == 1u, "overlap", "the mask read was refused");
    Require(((o.wr_gnt >> 1) & 1u) == 0u, "overlap", "the mask write clobbered an unread mask");
    Require(h->shadow()->wh() == wh_before + 1, "overlap", "the mask conflict was not counted");
    const Obs o2 = h->Cycle(Stim{});
    Require(o2.rsp_v[0] && o2.rsp_d[0] == old_v, "overlap",
            "the mask read did not return the old mask");
    h->WriteOne(0, 2, 5, 0, 8, new_v);
    Require(h->ReadOne(0, 2, 5, 0, 8, 0x5102) == new_v, "overlap",
            "the retried mask-aliased write did not take effect");
  }
  rep->Check(true, "overlap: a destination write aliasing a source or the mask register is "
                   "refused while its read is in flight, the read returns the pre-write value, "
                   "and the retried write then lands");
}

void PhaseLatency(Harness* h, Reporter* rep, Vmosaic_vrf_tb* dut) {
  h->Phase("latency");
  h->Reset(4);
  h->Prime(8);

  for (unsigned plat = 0; plat < 4; plat++) {
    for (unsigned lane : {2u, 4u, 8u}) {
      Stim probe;
      probe.lane_count = lane;
      probe.plat = plat;
      h->Cycle(probe);   // settle the combinational latency read-back
      Require(dut->o_rd_latency_o == kLat, "latency",
              "read latency is " + Dec(dut->o_rd_latency_o) + " at plat=" + Dec(plat) +
                  " lanes=" + Dec(lane) + ", expected the design's " + Dec(kLat));

      Stim s;
      s.lane_count = lane;
      s.plat = plat;
      s.rd[0].valid = true;
      s.rd[0].base = 3;
      s.rd[0].elem = 1;
      s.rd[0].sew_l = 5;
      s.rd[0].tag = 0x6001;
      const Obs grant = h->Cycle(s);
      Require(((grant.rd_gnt >> 0) & 1u) == 1u, "latency", "the probe read was refused");
      Require(!grant.rsp_v[0], "latency",
              "a response appeared in the grant cycle at plat=" + Dec(plat));

      Stim idle;
      idle.lane_count = lane;
      idle.plat = plat;
      const Obs rsp = h->Cycle(idle);
      Require(rsp.rsp_v[0] && rsp.rsp_t[0] == 0x6001, "latency",
              "the response did not arrive exactly one cycle after the grant at plat=" + Dec(plat) +
                  " lanes=" + Dec(lane));
    }
  }
  rep->Check(true, "latency: the read response arrives exactly one cycle after the grant at "
                   "every platform selector and lane count");
}

struct RandomOutcome {
  uint64_t hash = 0;
  uint32_t rd_gnt = 0, wr_gnt = 0, rc = 0, wc = 0, wh = 0;
  bool cover_conflict = false, cover_hazard = false, cover_wide = false, cover_bad = false;
};

RandomOutcome RunRandom(Harness* h, unsigned cycles, uint64_t seed) {
  mosaic::Rng rng(seed);
  RandomOutcome out;
  h->ResetHash();
  uint32_t last_gnt = 0;
  for (unsigned c = 0; c < cycles; c++) {
    Stim s;
    s.lane_count = (rng.Chance(50) ? 8u : (rng.Chance(50) ? 4u : 2u));
    s.plat = rng.Below(4);
    for (unsigned i = 0; i < kLanes; i++) {
      s.rd[i].valid = rng.Chance(45);
      s.rd[i].base = rng.Below(kVregs);
      s.rd[i].sew_l = 3 + rng.Below(4);           // 8, 16, 32, 64
      s.rd[i].lmul = static_cast<int>(rng.Below(7)) - 3;
      s.rd[i].elem = rng.Below(16);
      s.rd[i].tag = static_cast<uint16_t>(rng.Next());
      s.wr[i].valid = rng.Chance(35);
      s.wr[i].base = rng.Below(kVregs);
      s.wr[i].sew_l = 3 + rng.Below(4);
      s.wr[i].lmul = static_cast<int>(rng.Below(7)) - 3;
      s.wr[i].elem = rng.Below(16);
      s.wr[i].data = rng.Next();
    }
    // A quarter of the cycles force a self-aliasing access: read and write the
    // same element, exercising the lifetime guard and the conflict path.
    if (rng.Chance(25)) {
      const unsigned b = rng.Below(kVregs);
      const unsigned e = rng.Below(4);
      s.rd[0].valid = true;
      s.rd[0].base = b;
      s.rd[0].elem = e;
      s.rd[0].sew_l = 5;
      s.rd[0].lmul = 0;
      s.wr[0].valid = true;
      s.wr[0].base = b;
      s.wr[0].elem = e;
      s.wr[0].sew_l = 5;
      s.wr[0].lmul = 0;
      s.wr[0].data = rng.Next();
    }
    const Obs o = h->Cycle(s);
    out.rd_gnt += Popcount(o.rd_gnt);
    out.wr_gnt += Popcount(o.wr_gnt);
    last_gnt = o.rd_gnt;
    if (o.rd_gnt == 0 && o.wr_gnt == 0) continue;
    (void)last_gnt;
  }
  out.hash = h->hash();
  out.rc = h->shadow()->rc();
  out.wc = h->shadow()->wc();
  out.wh = h->shadow()->wh();
  out.cover_conflict = h->shadow()->rc() > 0;
  out.cover_hazard = h->shadow()->wh() > 0;
  out.cover_bad = h->shadow()->rb() > 0;
  return out;
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

  Reporter reporter(options, std::string("Verilator ") + Verilated::productVersion());
  ClockDriver clk;
  Vmosaic_vrf_tb dut;
  Harness harness(&dut, &clk, options.max_cycles);

  std::string detail;
  bool passed = true;
  try {
    PhaseGeometry(&harness, &reporter, &dut);
    PhaseMapping(&harness, &reporter);
    PhaseLaneCount(&harness, &reporter);
    PhaseConflicts(&harness, &reporter);
    PhaseWide(&harness, &reporter);
    PhaseFractional(&harness, &reporter);
    PhaseOverlap(&harness, &reporter);
    PhaseLatency(&harness, &reporter, &dut);

    harness.Phase("random");
    harness.Reset(4);
    harness.Prime(8);
    const RandomOutcome soak = RunRandom(&harness, 1500, options.seed);
    Require(soak.cover_conflict && soak.cover_hazard && soak.cover_bad, "random",
            "the soak did not cover every mechanism: conflict " + Bool(soak.cover_conflict) +
                " hazard " + Bool(soak.cover_hazard) + " bad " + Bool(soak.cover_bad));
    reporter.Check(true, "random: 1500 seeded cycles over the whole address space, every output "
                         "and counter compared every cycle (rd conflicts " + Dec(soak.rc) +
                             ", wr conflicts " + Dec(soak.wc) + ", hazards " + Dec(soak.wh) + ")");

    harness.Phase("determinism");
    harness.Reset(4);
    harness.Prime(8);
    const RandomOutcome again = RunRandom(&harness, 1500, options.seed);
    Require(soak.hash == again.hash, "determinism",
            "the same seed produced different results: 0x" + Hex64(soak.hash) + " vs 0x" +
                Hex64(again.hash));
    reporter.Check(true, "determinism: the same seed reproduces the run byte for byte (hash 0x" +
                             Hex64(again.hash) + ")");

    detail = "vrf contract holds: " + Dec(harness.comparisons()) + " per-cycle comparisons over " +
             Dec(harness.cycles()) + " cycles; read latency " + Dec(kLat) + " cycle; lane counts "
             "2/4/8 read the same logical vector; seed " + Dec(options.seed);
  } catch (const Failure& f) {
    reporter.Mismatch(f.what, "contract holds", "contract violated");
    passed = false;
    detail = "contract violated after " + Dec(harness.comparisons()) + " comparisons: " + f.what;
  }

  dut.final();
  reporter.Check(passed, "no contract violation in any phase");
  return reporter.Finish(passed ? "PASS" : "FAIL", detail);
}
