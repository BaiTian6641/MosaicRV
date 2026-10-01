// ============================================================================
// tb_prf.cpp -- CASE=prf.read_bank_collision, work package I-015.
//
// The DUT is never its own oracle. Every output and every piece of stored state
// is compared against an independent C++ shadow written from the *contract*
// documented in rtl/core/mosaic_prf.sv -- not from the RTL's structure. The
// shadow holds its own per-bank storage, its own validity bits, its own
// generation and data words and its own counters, and it derives the bank/row
// decode, the bank arbitration, the write-through rule, the admission rule and
// the never-written rule from that prose. It shares no code with the RTL and
// never looks at Verilator internals, so agreeing with it is evidence about the
// contract rather than a restatement of the implementation.
//
// Geometry is read from the elaborated DUT (`o_entries` and friends), so this
// file contains no register-file depth, no bank count, no row count and no tag
// width: a profile with different values needs no edit here, and the shadow
// cannot disagree with the hardware about how big it is. The first checks after
// reset assert that the numbers describe a coherent register file -- rows equal
// `ceil(entries/banks)`, the decode widths equal the `$clog2` of the counts, and
// the tag field wide enough to name every entry -- so a geometry that could not
// be modelled fails immediately rather than being modelled wrongly.
//
// Phases, each of which resets first and can fail on its own:
//
//   1. reset-state       the cold state: no entry has a value, the counters are
//                        zero, and a read of every entry reports never-written.
//   2. bank-collision    two demands to one bank in one cycle: exactly one is
//                        granted, the other is refused and then served when
//                        re-offered, and demands to other banks proceed in the
//                        same cycle.
//   3. write-through     a read in the cycle of a write to the same entry returns
//                        the new value and the new generation; a write to another
//                        row of the same bank does not leak in.
//   4. generation        a read naming the wrong generation reports the *stored*
//                        one, and a read of an unwritten entry does not.
//   5. never-written     an entry with no value since reset reports exactly that
//                        and is never presented as a plausible zero, including
//                        for a tag with no home entry at all.
//   6. write-admission   a write is admitted only when its enable, its
//                        generation-valid bit, its port's bank and its range all
//                        agree; a refused write changes nothing.
//   7. reset-validity    reset clears validity (and only validity: the storage
//                        array is deliberately not reset, which is why the array
//                        is compared only where the validity bit is set).
//   8. random            a random programme of writes and demanded reads, with
//                        refused demands retried out of a queue, compared against
//                        the shadow on every cycle.
//   9. determinism       the same seeded programme run twice, byte-identical,
//                        and every demand drained.
//
// Standing invariants, checked on every compared cycle rather than in one place:
//
//   * No two granted demands in one cycle target the same bank. This is the
//     per-cycle property the whole card is about, and it is checked from the
//     DUT's grants, not the shadow's.
//   * A grant implies a response, and a refusal implies none: `rsp_valid_o[b]`
//     equals `rd_ready_o[b]`, so a consumer can never see a response for a
//     demand that was not taken, nor lose one that was.
//   * `o_busy` is high exactly when some offered demand was refused.
//   * A never-written response carries no data and no mismatch; a mismatch is
//     never reported for an entry with no value.
//   * Granted demands plus refused demands equal the demands offered, so no
//     demand is invented and none disappears.
//   * The validity bits match the shadow exactly (they are control state, fully
//     deterministic), and the generation and data words match wherever the
//     validity bit is set. The words of an invalid entry are deliberately NOT
//     compared: the arrays are not reset, so their contents are undefined until
//     first written, and comparing them would be comparing power-up noise.
//
// Two-snapshot discipline: the demand grants, responses and `o_busy` are read
// *before* the clock edge, because that is what the DUT presented in the cycle
// under test; the counters and the storage are read *after* the edge, because
// that is when the registers are true. The refused-demand queue is updated from
// the pre-edge grants for the same reason.
// ============================================================================

#include <verilated.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "sim_common.h"
#include "Vmosaic_prf_tb.h"

namespace {

// The driver's own bound on the number of bank slots it can drive. It is a
// driver-side array bound, not a geometry constant: the real number comes from
// the DUT, and a profile that exceeded it fails the geometry check rather than
// overrunning an array.
constexpr uint32_t kMaxBanks = 8;

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
std::string Hex64(uint64_t value) { return mosaic::Hex(value, 16); }

// ------------------------------------------------------ Verilator port helpers
//
// Verilator gives a port a C++ type chosen from its SystemVerilog width:
// CData/SData/IData/QData up to 64 bits and `VlWide<N>` above it. The overloads
// below let the driver read and write a packed field without knowing which of
// the two it got, so a profile change that moved a port across the 64-bit
// boundary would be a compile-time event rather than a silent truncation of the
// field a comparison depends on.

uint64_t FieldMask(uint32_t width) {
  return (width >= 64) ? ~0ull : ((1ull << width) - 1ull);
}

// The width a `$clog2` would return in RTL, checked so the driver's own
// arithmetic can be compared against the DUT's read-back geometry.
uint32_t Clog2(uint32_t value) {
  uint32_t width = 1;
  while ((1u << width) < value) width++;
  return width;
}

inline void PortZero(uint8_t& port) { port = 0; }
inline void PortZero(uint16_t& port) { port = 0; }
inline void PortZero(uint32_t& port) { port = 0; }
inline void PortZero(uint64_t& port) { port = 0; }
template <int N>
inline void PortZero(VlWide<N>& port) {
  for (int i = 0; i < N; i++) port[i] = 0;
}

inline void PortSet(uint8_t& port, uint32_t bit, uint32_t width, uint64_t value) {
  const uint64_t mask = FieldMask(width) << bit;
  const uint64_t whole =
      (static_cast<uint64_t>(port) & ~mask) | ((value & FieldMask(width)) << bit);
  port = static_cast<uint8_t>(whole);
}
inline void PortSet(uint16_t& port, uint32_t bit, uint32_t width, uint64_t value) {
  const uint64_t mask = FieldMask(width) << bit;
  const uint64_t whole =
      (static_cast<uint64_t>(port) & ~mask) | ((value & FieldMask(width)) << bit);
  port = static_cast<uint16_t>(whole);
}
inline void PortSet(uint32_t& port, uint32_t bit, uint32_t width, uint64_t value) {
  const uint64_t mask = FieldMask(width) << bit;
  const uint64_t whole =
      (static_cast<uint64_t>(port) & ~mask) | ((value & FieldMask(width)) << bit);
  port = static_cast<uint32_t>(whole);
}
inline void PortSet(uint64_t& port, uint32_t bit, uint32_t width, uint64_t value) {
  const uint64_t mask = FieldMask(width) << bit;
  port = (port & ~mask) | ((value & FieldMask(width)) << bit);
}
template <int N>
inline void PortSet(VlWide<N>& port, uint32_t bit, uint32_t width, uint64_t value) {
  for (uint32_t b = 0; b < width; b++) {
    const uint32_t index = bit + b;
    const uint32_t word = index / 32;
    const uint32_t off = index % 32;
    const uint32_t bit_value = static_cast<uint32_t>((value >> b) & 1ull);
    port[word] = (port[word] & ~(1u << off)) | (bit_value << off);
  }
}

inline uint64_t PortGet(uint8_t port, uint32_t bit, uint32_t width) {
  return (static_cast<uint64_t>(port) >> bit) & FieldMask(width);
}
inline uint64_t PortGet(uint16_t port, uint32_t bit, uint32_t width) {
  return (static_cast<uint64_t>(port) >> bit) & FieldMask(width);
}
inline uint64_t PortGet(uint32_t port, uint32_t bit, uint32_t width) {
  return (static_cast<uint64_t>(port) >> bit) & FieldMask(width);
}
inline uint64_t PortGet(uint64_t port, uint32_t bit, uint32_t width) {
  return (port >> bit) & FieldMask(width);
}
template <int N>
inline uint64_t PortGet(const VlWide<N>& port, uint32_t bit, uint32_t width) {
  uint64_t value = 0;
  for (uint32_t b = 0; b < width; b++) {
    const uint32_t index = bit + b;
    value |= static_cast<uint64_t>((port[index / 32] >> (index % 32)) & 1u) << b;
  }
  return value;
}

// --------------------------------------------------------------- the stimulus
struct Demand {
  bool valid = false;
  uint32_t tag = 0;
  uint32_t gen = 0;
};

struct WriteOffer {
  bool en = false;
  bool gen_valid = false;
  uint32_t tag = 0;
  uint32_t gen = 0;
  uint64_t data = 0;
};

struct Stim {
  std::array<WriteOffer, kMaxBanks> wr{};
  std::array<Demand, kMaxBanks> rd{};

  std::string str() const {
    std::string text = "[wr=";
    for (uint32_t b = 0; b < kMaxBanks; b++) {
      if (!wr[b].en) continue;
      text += "b" + Dec(b) + ":(t" + Dec(wr[b].tag) + ",g" + Dec(wr[b].gen) +
              (wr[b].gen_valid ? ",v)" : ",x)") + "=" + Hex64(wr[b].data) + " ";
    }
    text += " rd=";
    for (uint32_t b = 0; b < kMaxBanks; b++) {
      if (!rd[b].valid) continue;
      text += "s" + Dec(b) + ":(t" + Dec(rd[b].tag) + ",g" + Dec(rd[b].gen) + ") ";
    }
    return text + "]";
  }
};

// ------------------------------------------------------- what the DUT presented
//
// Filled from the DUT's own outputs, never from the shadow's prediction: a phase
// that reads these is asserting about the hardware. The response fields are
// pre-edge (they describe the cycle under test); the counters are post-edge
// (they are registers).
struct Observed {
  std::array<bool, kMaxBanks> ready{};
  std::array<bool, kMaxBanks> rsp_valid{};
  std::array<bool, kMaxBanks> mismatch{};
  std::array<bool, kMaxBanks> never{};
  std::array<uint32_t, kMaxBanks> tag{};
  std::array<uint32_t, kMaxBanks> gen{};
  std::array<uint64_t, kMaxBanks> data{};
  bool busy = false;
  uint32_t wr_ctr = 0;
  uint32_t rd_ctr = 0;
  uint32_t conflict_ctr = 0;
  uint32_t mismatch_ctr = 0;
  uint32_t invalid_ctr = 0;
};

// ------------------------------------------------------------------ the shadow
//
// An independent model of the documented contract. The storage is a vector of
// per-bank rows rather than a flat array, so a bank/row swap cannot cancel
// against the same swap in the RTL; the arbitration is a per-bank "taken" flag
// walked in slot order rather than a priority encoder, so it cannot share a
// priority-encoding bug with the hardware.
class ShadowPrf {
 public:
  struct Entry {
    bool valid = false;
    uint32_t gen = 0;
    uint64_t data = 0;
  };

  struct Slot {
    bool ready = false;
    bool valid = false;
    bool mismatch = false;
    bool never = false;
    uint32_t tag = 0;
    uint32_t gen = 0;
    uint64_t data = 0;
  };

  struct Prediction {
    std::vector<Slot> slots;
    std::array<bool, kMaxBanks> wr_applied{};
    uint32_t wr_hits = 0;
    uint32_t rd_grants = 0;
    uint32_t refusals = 0;
    uint32_t mismatches = 0;
    uint32_t invalids = 0;
    uint32_t bypasses = 0;
    bool busy = false;
  };

  ShadowPrf(uint32_t banks, uint32_t entries, uint32_t tag_w, uint32_t gen_w, uint32_t xlen)
      : banks_(banks),
        entries_(entries),
        tag_w_(tag_w),
        gen_w_(gen_w),
        xlen_(xlen),
        rows_((entries + banks - 1) / banks) {
    Reset();
  }

  // The documented cold state: every entry invalid, every counter zero.
  void Reset() {
    mem_.assign(banks_, std::vector<Entry>(rows_, Entry{}));
    wr_ctr_ = 0;
    rd_ctr_ = 0;
    conflict_ctr_ = 0;
    mismatch_ctr_ = 0;
    invalid_ctr_ = 0;
  }

  uint32_t banks() const { return banks_; }
  uint32_t entries() const { return entries_; }
  uint32_t tag_w() const { return tag_w_; }
  uint32_t gen_w() const { return gen_w_; }
  uint32_t xlen() const { return xlen_; }
  uint32_t rows() const { return rows_; }
  uint32_t slot_of(uint32_t bank, uint32_t row) const { return bank * rows_ + row; }

  // The decode the contract states: a modulo, not a field split.
  uint32_t bank_of(uint32_t tag) const { return tag % banks_; }
  uint32_t row_of(uint32_t tag) const { return tag / banks_; }
  bool in_range(uint32_t tag) const { return tag < entries_; }

  const std::vector<std::vector<Entry>>& mem() const { return mem_; }

  uint64_t wr_ctr() const { return wr_ctr_; }
  uint64_t rd_ctr() const { return rd_ctr_; }
  uint64_t conflict_ctr() const { return conflict_ctr_; }
  uint64_t mismatch_ctr() const { return mismatch_ctr_; }
  uint64_t invalid_ctr() const { return invalid_ctr_; }

  // What the DUT must present this cycle. Pure: it reads the pre-edge state and
  // the stimulus and mutates nothing.
  Prediction Eval(const Stim& s) const {
    Prediction p;
    p.slots.assign(banks_, Slot{});

    // A write is admitted when its enable is high, its generation is live, its
    // tag's home bank is the port it arrived on, and its tag is in range.
    for (uint32_t b = 0; b < banks_; b++) {
      const WriteOffer& w = s.wr[b];
      p.wr_applied[b] = w.en && w.gen_valid && (bank_of(w.tag) == b) && in_range(w.tag);
      if (p.wr_applied[b]) p.wr_hits++;
    }

    // One demand per bank per cycle, lowest slot index first. A refused demand
    // produces no response at all: the consumer re-offers it.
    std::vector<bool> bank_taken(banks_, false);
    for (uint32_t s_idx = 0; s_idx < banks_; s_idx++) {
      Slot& out = p.slots[s_idx];
      const Demand& d = s.rd[s_idx];
      out.tag = d.tag;
      if (!d.valid) continue;
      const uint32_t bank = bank_of(d.tag);
      if (bank_taken[bank]) {
        p.refusals++;
        continue;
      }
      bank_taken[bank] = true;
      out.ready = true;
      out.valid = true;
      p.rd_grants++;

      const uint32_t row = row_of(d.tag);
      const bool bypass = p.wr_applied[bank] && (row_of(s.wr[bank].tag) == row);
      if (bypass) p.bypasses++;

      if (in_range(d.tag) && (bypass || mem_[bank][row].valid)) {
        if (bypass) {
          out.gen = s.wr[bank].gen;
          out.data = s.wr[bank].data;
          out.mismatch = (s.wr[bank].gen != d.gen);
        } else {
          out.gen = mem_[bank][row].gen;
          out.data = mem_[bank][row].data;
          out.mismatch = (mem_[bank][row].gen != d.gen);
        }
        if (out.mismatch) p.mismatches++;
      } else {
        // No value since reset, or no home entry at all. Data and generation are
        // reported as zero -- deterministic values, not storage contents -- and
        // no mismatch is raised, because the stored generation of an unwritten
        // entry is not an identity.
        out.never = true;
        p.invalids++;
      }
    }
    p.busy = (p.refusals != 0);
    return p;
  }

  // Advance the model over one edge. The write-through of the *read* path is
  // computed inside Eval from the same cycle's offers, so it does not depend on
  // this update order; here the storage and the counters move.
  void Apply(const Stim& s) {
    const Prediction p = Eval(s);
    for (uint32_t b = 0; b < banks_; b++) {
      if (!p.wr_applied[b]) continue;
      const uint32_t row = row_of(s.wr[b].tag);
      mem_[b][row].valid = true;
      mem_[b][row].gen = s.wr[b].gen;
      mem_[b][row].data = s.wr[b].data;
    }
    wr_ctr_ += p.wr_hits;
    rd_ctr_ += p.rd_grants;
    conflict_ctr_ += p.refusals;
    mismatch_ctr_ += p.mismatches;
    invalid_ctr_ += p.invalids;
  }

 private:
  uint32_t banks_;
  uint32_t entries_;
  uint32_t tag_w_;
  uint32_t gen_w_;
  uint32_t xlen_;
  uint32_t rows_;
  std::vector<std::vector<Entry>> mem_;
  uint64_t wr_ctr_ = 0;
  uint64_t rd_ctr_ = 0;
  uint64_t conflict_ctr_ = 0;
  uint64_t mismatch_ctr_ = 0;
  uint64_t invalid_ctr_ = 0;
};

// ------------------------------------------------------------------ the harness
class Harness {
 public:
  Harness(Vmosaic_prf_tb* dut, mosaic::ClockDriver* clk, uint64_t max_cycles)
      : dut_(dut), clk_(clk), max_cycles_(max_cycles) {}

  void Phase(const std::string& name) { phase_ = name; }
  void BindShadow(ShadowPrf* shadow) { shadow_ = shadow; }
  ShadowPrf* shadow() const { return shadow_; }

  // The geometry the driver drives and the shadow models, taken from the DUT's
  // own read-back outputs in main(). It is stored here rather than re-read per
  // cycle so that a phase cannot accidentally drive a different shape than the
  // shadow models.
  void SetGeometry(uint32_t banks, uint32_t tag_w, uint32_t gen_w, uint32_t xlen) {
    banks_ = banks;
    tag_w_ = tag_w;
    gen_w_ = gen_w;
    xlen_ = xlen;
  }

  uint32_t banks() const { return banks_; }

  // Assert reset for `cycles` rising edges with no other stimulus. The shadow is
  // reset here rather than by the caller, because a reset in the middle of a
  // phase that reset only the DUT would leave the two models describing
  // different machines, and the next comparison would then report a
  // disagreement that neither model has.
  void Reset(int cycles) {
    for (int i = 0; i < cycles; i++) {
      Cycle(Stim{}, /*rst=*/true);
    }
    if (shadow_ != nullptr) shadow_->Reset();
  }

  // One full clock period. The order is the documented one:
  //
  //   1. drive the inputs with the clock low, and compare the *combinational*
  //      outputs -- grants, responses, `o_busy` -- against the shadow computed
  //      from the same pre-edge state;
  //   2. advance the shadow over the edge;
  //   3. apply the edge, then compare the registered state: the counters and the
  //      storage. Comparing those before the edge would compare yesterday's
  //      registers against tomorrow's model.
  //
  // Returns by value: a reference would alias a member the next call overwrites,
  // and a phase that held on to an answer across another Cycle would silently be
  // reading the *next* cycle's outputs.
  Observed Cycle(const Stim& s, bool rst = false) {
    if (cycles_ >= max_cycles_) {
      Fail(phase_, "max-cycles (" + Dec(max_cycles_) + ") exhausted before the phase finished");
    }
    const std::string where = phase_ + ": cycle " + Dec(clk_->cycle());

    Drive(s, rst);
    dut_->eval();

    Observed o;
    if (!rst) {
      Capture(&o);
      const ShadowPrf::Prediction p = shadow_->Eval(s);
      CompareOutputs(s, o, p, where);
      shadow_->Apply(s);
    }

    dut_->clk = 1;
    dut_->eval();
    clk_->Tick();
    dut_->clk = 0;
    dut_->eval();
    ++cycles_;

    if (!rst) {
      // Post-edge: the counters are registers, and the storage has moved.
      o.wr_ctr = dut_->o_wr_ctr;
      o.rd_ctr = dut_->o_rd_ctr;
      o.conflict_ctr = dut_->o_conflict_ctr;
      o.mismatch_ctr = dut_->o_mismatch_ctr;
      o.invalid_ctr = dut_->o_invalid_ctr;
      CompareState(where);
      CheckInvariants(s, o, where);
      ++comparisons_;
    }
    return o;
  }

  uint64_t comparisons() const { return comparisons_; }
  uint64_t cycles() const { return cycles_; }

  // ------------------------------------------------- whole-storage observation
  //
  // Slot index `bank*rows + row`, matching the wrapper's packing. The validity
  // bits and the words are read from the DUT's own storage, not from the
  // response path: a write that lands in the wrong row of the right bank and is
  // never read would otherwise be invisible.
  std::vector<bool> DutValidBits() const {
    std::vector<bool> out(banks_ * shadow_->rows(), false);
    for (uint32_t slot = 0; slot < out.size(); slot++) {
      out[slot] = (dut_->dbg_valid[slot / 32] >> (slot % 32)) & 1u;
    }
    return out;
  }

  std::vector<uint32_t> DutGens() const {
    std::vector<uint32_t> out(banks_ * shadow_->rows(), 0);
    for (uint32_t slot = 0; slot < out.size(); slot++) {
      out[slot] = static_cast<uint32_t>(PortGet(dut_->dbg_gen, slot * gen_w_, gen_w_));
    }
    return out;
  }

  std::vector<uint64_t> DutDatas() const {
    std::vector<uint64_t> out(banks_ * shadow_->rows(), 0);
    for (uint32_t slot = 0; slot < out.size(); slot++) {
      out[slot] = PortGet(dut_->dbg_data, slot * xlen_, xlen_);
    }
    return out;
  }

 private:
  void Drive(const Stim& s, bool rst) {
    dut_->clk = 0;
    dut_->rst = rst ? 1 : 0;

    uint32_t wr_en = 0;
    uint32_t wr_gen_valid = 0;
    uint32_t rd_valid = 0;
    for (uint32_t b = 0; b < banks_; b++) {
      if (s.wr[b].en) wr_en |= 1u << b;
      if (s.wr[b].gen_valid) wr_gen_valid |= 1u << b;
      if (s.rd[b].valid) rd_valid |= 1u << b;
    }
    dut_->wr_en_i = wr_en;
    dut_->wr_gen_valid_i = wr_gen_valid;
    dut_->rd_valid_i = rd_valid;

    PortZero(dut_->wr_tag_i);
    PortZero(dut_->wr_gen_i);
    PortZero(dut_->wr_data_i);
    PortZero(dut_->rd_tag_i);
    PortZero(dut_->rd_gen_i);
    for (uint32_t b = 0; b < banks_; b++) {
      PortSet(dut_->wr_tag_i, b * tag_w_, tag_w_, s.wr[b].tag);
      PortSet(dut_->wr_gen_i, b * gen_w_, gen_w_, s.wr[b].gen);
      PortSet(dut_->wr_data_i, b * xlen_, xlen_, s.wr[b].data);
      PortSet(dut_->rd_tag_i, b * tag_w_, tag_w_, s.rd[b].tag);
      PortSet(dut_->rd_gen_i, b * gen_w_, gen_w_, s.rd[b].gen);
    }
  }

  // Read the DUT's outputs as they stand *before* the edge: this is what the DUT
  // presented in the cycle under test.
  void Capture(Observed* o) {
    for (uint32_t b = 0; b < banks_; b++) {
      o->ready[b] = dut_->rd_ready_o & (1u << b);
      o->rsp_valid[b] = dut_->rsp_valid_o & (1u << b);
      o->mismatch[b] = dut_->rsp_gen_mismatch_o & (1u << b);
      o->never[b] = dut_->rsp_never_written_o & (1u << b);
      o->tag[b] = static_cast<uint32_t>(PortGet(dut_->rsp_tag_o, b * tag_w_, tag_w_));
      o->gen[b] = static_cast<uint32_t>(PortGet(dut_->rsp_gen_o, b * gen_w_, gen_w_));
      o->data[b] = PortGet(dut_->rsp_data_o, b * xlen_, xlen_);
    }
    o->busy = dut_->o_busy != 0;
  }

  void CompareOutputs(const Stim& s, const Observed& o, const ShadowPrf::Prediction& p,
                      const std::string& where) {
    const std::string stim = " " + s.str();
    for (uint32_t b = 0; b < banks_; b++) {
      const ShadowPrf::Slot& want = p.slots[b];
      const std::string lane = " slot " + Dec(b) + ":";
      Require(o.ready[b] == want.ready, where,
              lane + " rd_ready expected " + Bool(want.ready) + ", got " + Bool(o.ready[b]) + stim);
      Require(o.rsp_valid[b] == want.valid, where,
              lane + " rsp_valid expected " + Bool(want.valid) + ", got " +
                  Bool(o.rsp_valid[b]) + stim);
      Require(o.mismatch[b] == want.mismatch, where,
              lane + " rsp_gen_mismatch expected " + Bool(want.mismatch) + ", got " +
                  Bool(o.mismatch[b]) + stim);
      Require(o.never[b] == want.never, where,
              lane + " rsp_never_written expected " + Bool(want.never) + ", got " +
                  Bool(o.never[b]) + stim);
      Require(o.tag[b] == want.tag, where,
              lane + " rsp_tag expected " + Dec(want.tag) + ", got " + Dec(o.tag[b]) + stim);
      Require(o.gen[b] == want.gen, where,
              lane + " rsp_gen expected " + Dec(want.gen) + ", got " + Dec(o.gen[b]) + stim);
      Require(o.data[b] == want.data, where,
              lane + " rsp_data expected " + Hex64(want.data) + ", got " + Hex64(o.data[b]) +
                  stim);
    }
    Require(o.busy == p.busy, where,
            "o_busy expected " + Bool(p.busy) + ", got " + Bool(o.busy) + stim);
  }

  // Post-edge: the counters are registers and the storage has moved.
  void CompareState(const std::string& where) {
    auto ctr = [&](const char* name, uint32_t got, uint64_t want) {
      Require(got == static_cast<uint32_t>(want), where,
              std::string(name) + " expected " + Dec(static_cast<uint32_t>(want)) + ", got " +
                  Dec(got));
    };
    ctr("o_wr_ctr", dut_->o_wr_ctr, shadow_->wr_ctr());
    ctr("o_rd_ctr", dut_->o_rd_ctr, shadow_->rd_ctr());
    ctr("o_conflict_ctr", dut_->o_conflict_ctr, shadow_->conflict_ctr());
    ctr("o_mismatch_ctr", dut_->o_mismatch_ctr, shadow_->mismatch_ctr());
    ctr("o_invalid_ctr", dut_->o_invalid_ctr, shadow_->invalid_ctr());

    const std::vector<bool> valid = DutValidBits();
    const std::vector<uint32_t> gens = DutGens();
    const std::vector<uint64_t> datas = DutDatas();
    for (uint32_t b = 0; b < banks_; b++) {
      for (uint32_t r = 0; r < shadow_->rows(); r++) {
        const uint32_t slot = shadow_->slot_of(b, r);
        const ShadowPrf::Entry& want = shadow_->mem()[b][r];
        Require(valid[slot] == want.valid, where,
                "the validity bit of bank " + Dec(b) + " row " + Dec(r) + " expected " +
                    Bool(want.valid) + ", got " + Bool(valid[slot]));
        if (!want.valid) continue;
        // The word contents are compared only where the entry is valid: the
        // arrays are deliberately not reset, so an unwritten row holds whatever
        // the simulator powered up with and comparing it would compare noise.
        Require(gens[slot] == want.gen, where,
                "the stored generation of bank " + Dec(b) + " row " + Dec(r) + " expected " +
                    Dec(want.gen) + ", got " + Dec(gens[slot]));
        Require(datas[slot] == want.data, where,
                "the stored data of bank " + Dec(b) + " row " + Dec(r) + " expected " +
                    Hex64(want.data) + ", got " + Hex64(datas[slot]));
      }
    }
  }

  // The per-cycle properties, checked from the DUT's grants and the stimulus the
  // DUT was given. `o` was captured before the edge on purpose: the grants
  // describe the cycle under test.
  void CheckInvariants(const Stim& s, const Observed& o, const std::string& where) {
    const std::string stim = " " + s.str();

    // 1. No two granted demands in one cycle target the same bank. This is the
    //    card's per-cycle property, and it is asserted from the hardware's own
    //    grants.
    for (uint32_t a = 0; a < banks_; a++) {
      if (!o.ready[a]) continue;
      Require(s.rd[a].valid, where,
              "slot " + Dec(a) + " was granted although no demand was offered on it" + stim);
      for (uint32_t b = a + 1; b < banks_; b++) {
        if (!o.ready[b]) continue;
        Require(shadow_->bank_of(s.rd[a].tag) != shadow_->bank_of(s.rd[b].tag), where,
                "slots " + Dec(a) + " and " + Dec(b) +
                    " were both granted in one cycle and both demand bank " +
                    Dec(shadow_->bank_of(s.rd[a].tag)) +
                    ": one bank's single read port served two reads" + stim);
      }
    }

    // 2. Every granted demand is answered, and only granted demands are.
    uint32_t offered = 0;
    uint32_t granted = 0;
    uint32_t refused = 0;
    for (uint32_t b = 0; b < banks_; b++) {
      if (s.rd[b].valid) offered++;
      if (o.ready[b]) granted++;
      if (s.rd[b].valid && !o.ready[b]) refused++;
      Require(o.rsp_valid[b] == o.ready[b], where,
              "slot " + Dec(b) + " response valid " + Bool(o.rsp_valid[b]) + " does not match its "
              "grant " + Bool(o.ready[b]) + stim);
      // A refused demand must not be answered with a value, and a grant must not
      // be answered with "no value" unless the entry genuinely has none.
      if (!o.ready[b]) {
        Require(!o.mismatch[b] && !o.never[b], where,
                "slot " + Dec(b) + " was refused but still reported a mismatch or a "
                "never-written entry" + stim);
      }
    }
    Require(granted + refused == offered, where,
            "granted " + Dec(granted) + " plus refused " + Dec(refused) +
                " demands do not equal the " + Dec(offered) + " demands offered" + stim);
    Require(o.busy == (refused != 0), where,
            "o_busy is " + Bool(o.busy) + " with " + Dec(refused) +
                " refused demands in this cycle" + stim);

    // 3. A response for an entry with no value carries no data and no mismatch.
    for (uint32_t b = 0; b < banks_; b++) {
      if (o.never[b]) {
        Require(!o.mismatch[b], where,
                "slot " + Dec(b) + " reported both a generation mismatch and a never-written "
                "entry" + stim);
        Require(o.data[b] == 0, where,
                "slot " + Dec(b) + " reported a never-written entry but data " +
                    Hex64(o.data[b]) + ": an unwritten entry was presented as a value" + stim);
      }
    }
  }

  Vmosaic_prf_tb* dut_;
  mosaic::ClockDriver* clk_;
  uint64_t max_cycles_;
  ShadowPrf* shadow_ = nullptr;
  std::string phase_;
  uint32_t banks_ = 0;
  uint32_t tag_w_ = 0;
  uint32_t gen_w_ = 0;
  uint32_t xlen_ = 0;
  uint64_t comparisons_ = 0;
  uint64_t cycles_ = 0;
};

// -------------------------------------------------------------------- phases

// Phase 1: the documented cold state -- nothing has a value, every counter is
// zero, and a read of every entry says so instead of returning a plausible zero.
void PhaseResetState(Harness* h, mosaic::Reporter* reporter, uint32_t entries,
                     uint32_t banks) {
  const Stim none;
  const Observed cold = h->Cycle(none);
  Require(cold.wr_ctr == 0 && cold.rd_ctr == 0 && cold.conflict_ctr == 0 &&
              cold.mismatch_ctr == 0 && cold.invalid_ctr == 0,
          "reset-state", "the counters are not zero after reset");
  Require(!cold.busy, "reset-state", "o_busy is high with no demand offered");
  for (uint32_t b = 0; b < banks; b++) {
    Require(!cold.ready[b] && !cold.rsp_valid[b], "reset-state",
            "slot " + Dec(b) + " reported a response with no demand offered");
  }

  const std::vector<bool> valid = h->DutValidBits();
  for (uint32_t slot = 0; slot < valid.size(); slot++) {
    Require(!valid[slot], "reset-state",
            "storage slot " + Dec(slot) +
                " is valid right after reset: validity is control state and must be cleared");
  }

  // Every entry, in banks-wide groups. Tag `base + k` has home bank `k` because
  // `base` is a multiple of the bank count, so each group is a set of distinct
  // banks and every demand in it must be granted in the same cycle.
  uint32_t read_entries = 0;
  Observed last;
  for (uint32_t base = 0; base < entries; base += banks) {
    Stim s;
    for (uint32_t k = 0; k < banks && base + k < entries; k++) {
      s.rd[k].valid = true;
      s.rd[k].tag = base + k;
      s.rd[k].gen = 0;
    }
    last = h->Cycle(s);
    for (uint32_t k = 0; k < banks && base + k < entries; k++) {
      const std::string where = "reset-state: entry " + Dec(base + k);
      Require(last.ready[k], where, "the demand was refused although no other slot used its bank");
      Require(last.never[k], where,
              "a read of an entry that has never been written did not report "
              "rsp_never_written_o: an uninitialised read is being presented as a value");
      Require(!last.mismatch[k], where,
              "a never-written entry reported a generation mismatch, which compares against "
              "storage that is not an identity");
      Require(last.data[k] == 0 && last.gen[k] == 0, where,
              "a never-written entry returned a generation or data value");
      read_entries++;
    }
  }
  Require(read_entries == entries, "reset-state",
          "read " + Dec(read_entries) + " entries, expected " + Dec(entries));
  Require(last.wr_ctr == 0, "reset-state", "a write was admitted although none was offered");
  Require(last.rd_ctr == entries, "reset-state",
          "o_rd_ctr is " + Dec(last.rd_ctr) + " after reading " + Dec(entries) + " entries");
  Require(last.invalid_ctr == entries, "reset-state",
          "o_invalid_ctr is " + Dec(last.invalid_ctr) + " after " + Dec(entries) +
              " never-written reads");
  Require(last.conflict_ctr == 0 && last.mismatch_ctr == 0, "reset-state",
          "a conflict or a generation mismatch was counted with no conflicting demand and no "
          "written entry");

  reporter->Check(true, "reset-state: the cold state is invalid-and-zero, not zero-and-valid");
}

// Phase 2: two demands to one bank in one cycle. Exactly one is granted, the
// other is refused (and reported as refused), demands to other banks proceed in
// the same cycle, and the refused demand is served when it is re-offered.
void PhaseBankCollision(Harness* h, mosaic::Reporter* reporter, uint32_t entries,
                        uint32_t banks) {
  const ShadowPrf* sh = h->shadow();
  Require(entries > banks, "bank-collision",
          "the geometry has no second tag in bank 0 (entries=" + Dec(entries) +
              ", banks=" + Dec(banks) + "), so a bank conflict cannot be presented");
  const uint32_t tag_a = 0;
  const uint32_t tag_b = banks;  // the same home bank, a different row
  Require(sh->bank_of(tag_a) == 0 && sh->bank_of(tag_b) == 0 && tag_a != tag_b,
          "bank-collision", "the driver's own decode does not place both tags in bank 0");

  const uint32_t conflicts_before = sh->conflict_ctr();
  Stim s;
  s.rd[0].valid = true;
  s.rd[0].tag = tag_a;
  s.rd[1].valid = true;
  s.rd[1].tag = tag_b;
  for (uint32_t k = 2; k < banks; k++) {
    s.rd[k].valid = true;
    s.rd[k].tag = k - 1;  // home bank k-1, distinct from 0 and from each other
  }
  const Observed o = h->Cycle(s);

  Require(o.ready[0], "bank-collision",
          "slot 0's demand was not granted, although nothing else used its bank first");
  Require(!o.ready[1], "bank-collision",
          "slot 1 demanded the same bank as slot 0 in the same cycle and was granted: one bank's "
          "single read port served two reads");
  Require(!o.rsp_valid[1] && !o.mismatch[1] && !o.never[1], "bank-collision",
          "the refused demand was answered anyway: a refused demand must carry no response");
  for (uint32_t k = 2; k < banks; k++) {
    Require(o.ready[k], "bank-collision",
            "slot " + Dec(k) + " demands bank " + Dec(k - 1) +
                " while slots 0 and 1 contend for bank 0, so it must be granted in the same "
                "cycle: a conflict on one bank must not stall the others");
  }
  Require(o.busy, "bank-collision",
          "o_busy is low in a cycle in which a demand was refused");
  Require(o.conflict_ctr == conflicts_before + 1, "bank-collision",
          "o_conflict_ctr is " + Dec(o.conflict_ctr) + ", expected exactly one more than " +
              Dec(conflicts_before));
  Require(o.rd_ctr == banks - 1, "bank-collision",
          "o_rd_ctr is " + Dec(o.rd_ctr) + " after " + Dec(banks - 1) + " grants");

  // The refused demand is re-offered by itself and must be served. A register
  // file that refused it and forgot it would hang the consumer with no report.
  Stim retry;
  retry.rd[1] = s.rd[1];
  const Observed o2 = h->Cycle(retry);
  Require(o2.ready[1] && o2.rsp_valid[1], "bank-collision",
          "the refused demand was not served when re-offered by itself: it was dropped rather "
          "than refused");
  Require(o2.never[1], "bank-collision",
          "the re-offered demand named an entry with no value since reset, so the response must "
          "report rsp_never_written_o");
  Require(!o2.busy, "bank-collision", "o_busy is high in a cycle with no refused demand");

  // Three demands to one bank: still exactly one grant, and both refusals are
  // still recoverable.
  if (entries > 2 * banks) {
    Stim t;
    t.rd[0].valid = true;
    t.rd[0].tag = tag_a;
    t.rd[1].valid = true;
    t.rd[1].tag = tag_b;
    t.rd[2].valid = true;
    t.rd[2].tag = 2 * banks;  // home bank 0 again
    const Observed o3 = h->Cycle(t);
    uint32_t granted = 0;
    for (uint32_t b = 0; b < banks; b++) {
      if (o3.ready[b]) granted++;
    }
    Require(granted == 1, "bank-collision",
            "three demands to one bank in one cycle produced " + Dec(granted) + " grants");
    Require(o3.conflict_ctr == o2.conflict_ctr + 2, "bank-collision",
            "the two refused demands were not both counted as conflicts");

    Stim retry2;
    retry2.rd[1] = t.rd[1];
    const Observed o4 = h->Cycle(retry2);
    Require(o4.ready[1], "bank-collision", "the second refused demand was lost");
    Stim retry3;
    retry3.rd[2] = t.rd[2];
    const Observed o5 = h->Cycle(retry3);
    Require(o5.ready[2], "bank-collision", "the third refused demand was lost");
  }

  reporter->Check(true, "bank-collision: one grant per bank per cycle, refusals reported and "
                        "then served");
}

// Phase 3: same-cycle write and read of one entry.
void PhaseWriteThrough(Harness* h, mosaic::Reporter* reporter, uint32_t entries,
                       uint32_t banks, uint32_t xlen) {
  const ShadowPrf* sh = h->shadow();
  const uint64_t mask = FieldMask(xlen);
  const uint32_t tag_a = 0;      // home bank 0
  const uint32_t tag_b = banks;  // home bank 0, a different row
  Require(entries > banks, "write-through",
          "the geometry has no second tag in bank 0, so the same-bank/different-row rule "
          "cannot be presented");
  const uint32_t bank_a = sh->bank_of(tag_a);
  const uint64_t d_old = 0x0123456789abcdefull & mask;
  const uint64_t d_new = 0xfedcba9876543210ull & mask;
  const uint64_t d_other = 0x0f0f0f0f0f0f0f0full & mask;

  // 1. Preload, so the old value is a real value rather than "no value".
  Stim w;
  w.wr[bank_a].en = true;
  w.wr[bank_a].gen_valid = true;
  w.wr[bank_a].tag = tag_a;
  w.wr[bank_a].gen = 3;
  w.wr[bank_a].data = d_old;
  const Observed o1 = h->Cycle(w);
  Require(o1.wr_ctr == 1, "write-through", "the preload was not admitted");

  Stim r;
  r.rd[0].valid = true;
  r.rd[0].tag = tag_a;
  r.rd[0].gen = 3;
  const Observed o2 = h->Cycle(r);
  Require(o2.ready[0] && !o2.never[0] && !o2.mismatch[0] && o2.data[0] == d_old && o2.gen[0] == 3,
          "write-through",
          "an entry that was just written did not read back: expected " + Hex64(d_old) +
              "/gen 3, got " + Hex64(o2.data[0]) + "/gen " + Dec(o2.gen[0]));

  // 2. Same cycle: write the entry and read it. The read must see the new value
  //    and the new generation, and its comparison must be against the new one.
  //    The read is offered in a slot that is *not* the bank index, to pin down
  //    that the slot number does not select a bank -- the tag does.
  const uint32_t read_slot = (bank_a + 1) % banks;
  Stim sr;
  sr.wr[bank_a].en = true;
  sr.wr[bank_a].gen_valid = true;
  sr.wr[bank_a].tag = tag_a;
  sr.wr[bank_a].gen = 4;
  sr.wr[bank_a].data = d_new;
  sr.rd[read_slot].valid = true;
  sr.rd[read_slot].tag = tag_a;
  sr.rd[read_slot].gen = 4;
  const Observed o3 = h->Cycle(sr);
  Require(o3.ready[read_slot] && o3.rsp_valid[read_slot], "write-through",
          "the read of the entry being written in the same cycle was not granted");
  Require(o3.data[read_slot] == d_new, "write-through",
          "a same-cycle write to the same entry was not visible to the read: expected " +
              Hex64(d_new) + ", got " + Hex64(o3.data[read_slot]));
  Require(o3.data[read_slot] != d_old, "write-through",
          "the read returned the value the write replaced: a consumer would compute with a "
          "stale operand in the cycle the wakeup said the value had arrived");
  Require(o3.gen[read_slot] == 4 && !o3.mismatch[read_slot] && !o3.never[read_slot],
          "write-through",
          "a same-cycle write did not carry its new generation into the response: got gen " +
              Dec(o3.gen[read_slot]) + " mismatch " + Bool(o3.mismatch[read_slot]));

  // 3. The storage holds the new value from the next cycle on.
  Stim r2;
  r2.rd[0].valid = true;
  r2.rd[0].tag = tag_a;
  r2.rd[0].gen = 4;
  const Observed o4 = h->Cycle(r2);
  Require(o4.data[0] == d_new && o4.gen[0] == 4 && !o4.mismatch[0], "write-through",
          "the write-through value did not reach the storage: expected " + Hex64(d_new) +
              ", got " + Hex64(o4.data[0]));

  // 4. A write to a *different row of the same bank* must not leak into the
  //    response: the bypass compares entries, not banks.
  Stim sb;
  sb.wr[bank_a].en = true;
  sb.wr[bank_a].gen_valid = true;
  sb.wr[bank_a].tag = tag_b;
  sb.wr[bank_a].gen = 5;
  sb.wr[bank_a].data = d_other;
  sb.rd[0].valid = true;
  sb.rd[0].tag = tag_a;
  sb.rd[0].gen = 4;
  const Observed o5 = h->Cycle(sb);
  Require(o5.data[0] == d_new, "write-through",
          "a write to another row of the same bank leaked into the response: the bypass "
          "compares banks instead of entries (got " + Hex64(o5.data[0]) + ")");

  // 5. A write whose generation is not live is not admitted, and therefore does
  //    not bypass either.
  Stim gv;
  gv.wr[bank_a].en = true;
  gv.wr[bank_a].gen_valid = false;
  gv.wr[bank_a].tag = tag_a;
  gv.wr[bank_a].gen = 6;
  gv.wr[bank_a].data = d_other;
  gv.rd[0].valid = true;
  gv.rd[0].tag = tag_a;
  gv.rd[0].gen = 4;
  const Observed o6 = h->Cycle(gv);
  Require(o6.data[0] == d_new && !o6.mismatch[0] && !o6.never[0] && o6.gen[0] == 4,
          "write-through",
          "a write with no live generation changed the response: an entry was written through "
          "a generation that was never assigned");
  Require(o6.wr_ctr == o5.wr_ctr, "write-through",
          "a write with no live generation was counted as admitted");

  reporter->Check(true, "write-through: new value and new generation in the write cycle, "
                        "same entry only");
}

// Phase 4: generation identity.
void PhaseGeneration(Harness* h, mosaic::Reporter* reporter, uint32_t xlen) {
  const ShadowPrf* sh = h->shadow();
  const uint64_t mask = FieldMask(xlen);
  const uint32_t tag = 1;  // home bank 1 (with any bank count above one; bank 0 for a single bank)
  const uint64_t data = 0xdeadbeefcafef00dull & mask;

  Stim w;
  w.wr[sh->bank_of(tag)].en = true;
  w.wr[sh->bank_of(tag)].gen_valid = true;
  w.wr[sh->bank_of(tag)].tag = tag;
  w.wr[sh->bank_of(tag)].gen = 7;
  w.wr[sh->bank_of(tag)].data = data;
  h->Cycle(w);

  // The matching read reports no mismatch.
  Stim ok;
  ok.rd[0].valid = true;
  ok.rd[0].tag = tag;
  ok.rd[0].gen = 7;
  const Observed o1 = h->Cycle(ok);
  Require(o1.ready[0] && !o1.mismatch[0] && !o1.never[0] && o1.gen[0] == 7 && o1.data[0] == data,
          "generation",
          "a read naming the stored generation was reported as a mismatch or lost the value");

  // The read from the previous incarnation reports the *stored* generation.
  const uint32_t before = o1.mismatch_ctr;
  Stim stale;
  stale.rd[0].valid = true;
  stale.rd[0].tag = tag;
  stale.rd[0].gen = 6;
  const Observed o2 = h->Cycle(stale);
  Require(o2.mismatch[0], "generation",
          "a read naming generation 6 of an entry holding generation 7 reported no mismatch: a "
          "stale operand would be consumed as a live one");
  Require(o2.gen[0] == 7, "generation",
          "a mismatched read reported generation " + Dec(o2.gen[0]) +
              " instead of the stored generation 7: the consumer cannot compare against a "
              "generation the register file did not report");
  Require(!o2.never[0], "generation",
          "an entry that holds a value was reported as never written");
  Require(o2.gen[0] != stale.rd[0].gen, "generation",
          "the response echoed the requested generation instead of the stored one");
  Require(o2.mismatch_ctr == before + 1, "generation",
          "o_mismatch_ctr did not move by exactly one across one mismatched read");

  // A mismatched read keeps the counter and the never-written report separate:
  // the entry has a value, it is just not the requested one.
  const uint32_t invalid_before = o2.invalid_ctr;
  Stim stale2;
  stale2.rd[0].valid = true;
  stale2.rd[0].tag = tag;
  stale2.rd[0].gen = 0;
  const Observed o3 = h->Cycle(stale2);
  Require(o3.mismatch[0] && !o3.never[0], "generation",
          "generation 0 of a written entry was treated as an unwritten entry: generation 0 is "
          "a real generation");
  Require(o3.invalid_ctr == invalid_before, "generation",
          "a mismatched read of a written entry incremented o_invalid_ctr");

  reporter->Check(true, "generation: the stored generation is reported, not the requested one");
}

// Phase 5: an entry with no value since reset must say so, including one whose
// tag has no home entry at all.
void PhaseNeverWritten(Harness* h, mosaic::Reporter* reporter, uint32_t entries,
                       uint32_t banks, uint32_t tag_w, uint32_t gen_w) {
  const ShadowPrf* sh = h->shadow();

  // A tag that no write in this phase touches. Tag `entries - 1` has home bank
  // (entries-1) % banks; it is untouched because nothing has been written since
  // the last reset.
  const uint32_t tag = entries - 1;
  const uint32_t slot = sh->bank_of(tag) % banks;
  const uint32_t gen = (1u << gen_w) - 1;
  Stim r;
  r.rd[slot].valid = true;
  r.rd[slot].tag = tag;
  r.rd[slot].gen = gen;
  const Observed o = h->Cycle(r);
  Require(o.ready[slot] && o.rsp_valid[slot], "never-written",
          "the demand on an unwritten entry was refused: a consumer would hang with no report");
  Require(o.never[slot], "never-written",
          "a read of an entry that has never been written did not raise rsp_never_written_o: "
          "an uninitialised read is being presented as a value");
  Require(!o.mismatch[slot], "never-written",
          "a never-written entry reported a generation mismatch against storage that is not an "
          "identity");
  Require(o.data[slot] == 0 && o.gen[slot] == 0, "never-written",
          "a never-written entry returned data " + Hex64(o.data[slot]) + "/gen " +
              Dec(o.gen[slot]) + " instead of an explicit no-value report");
  Require(o.invalid_ctr == 1, "never-written",
          "o_invalid_ctr is " + Dec(o.invalid_ctr) + " after one never-written read");
  Require(o.rd_ctr == 1, "never-written",
          "a never-written read is still a served demand, so o_rd_ctr must be 1, not " +
              Dec(o.rd_ctr));

  // A tag with no home entry at all: the tag field can name more tags than the
  // register file has entries, and such a tag must be answered rather than
  // silently wrapped onto a real entry.
  if ((1u << tag_w) > entries) {
    const uint32_t out_of_range = entries;  // no home entry
    Require(sh->bank_of(out_of_range) < banks, "never-written", "decode sanity");
    Stim oor;
    oor.rd[0].valid = true;
    oor.rd[0].tag = out_of_range;
    oor.rd[0].gen = 0;
    const Observed o2 = h->Cycle(oor);
    Require(o2.ready[0] && o2.rsp_valid[0], "never-written",
            "a demand for a tag with no home entry was refused rather than answered: the "
            "consumer would hang");
    Require(o2.never[0], "never-written",
            "a tag with no home entry was reported as holding a value: a read was wrapped onto "
            "a real entry");
    Require(o2.data[0] == 0 && !o2.mismatch[0], "never-written",
            "a tag with no home entry returned data or a mismatch");
  }

  reporter->Check(true, "never-written: no value is ever presented as a value");
}

// Phase 6: write admission -- enable, live generation, home bank and range.
void PhaseWriteAdmission(Harness* h, mosaic::Reporter* reporter, uint32_t entries,
                         uint32_t banks, uint32_t tag_w) {
  const ShadowPrf* sh = h->shadow();

  // A write offered on the wrong bank's port is refused. The tag is real, its
  // home bank is not the port it arrived on, and admitting it would put the
  // value in another bank's row space where no read for that tag would find it.
  const uint32_t tag = 0;                        // home bank 0
  const uint32_t wrong_port = banks - 1;         // anything but 0 (banks > 1 here)
  Require(banks > 1 && sh->bank_of(tag) == 0 && wrong_port != sh->bank_of(tag),
          "write-admission", "the geometry cannot express a mis-routed write");
  Stim misroute;
  misroute.wr[wrong_port].en = true;
  misroute.wr[wrong_port].gen_valid = true;
  misroute.wr[wrong_port].tag = tag;
  misroute.wr[wrong_port].gen = 2;
  misroute.wr[wrong_port].data = 0x1111ull;
  const Observed o1 = h->Cycle(misroute);
  Require(o1.wr_ctr == 0, "write-admission",
          "a write whose tag's home bank is not its port was admitted");

  Stim r1;
  r1.rd[0].valid = true;
  r1.rd[0].tag = tag;
  r1.rd[0].gen = 2;
  const Observed o2 = h->Cycle(r1);
  Require(o2.never[0], "write-admission",
          "the mis-routed write landed in the register file: the entry it named now reports a "
          "value");

  // A write with no live generation is refused.
  Stim dead_gen;
  dead_gen.wr[0].en = true;
  dead_gen.wr[0].gen_valid = false;
  dead_gen.wr[0].tag = tag;
  dead_gen.wr[0].gen = 2;
  dead_gen.wr[0].data = 0x2222ull;
  const Observed o3 = h->Cycle(dead_gen);
  Require(o3.wr_ctr == 0, "write-admission",
          "a write with no live generation was admitted: a producer manufactured validity out "
          "of a generation that was never assigned");

  // A write to a tag with no home entry is refused.
  if ((1u << tag_w) > entries) {
    Stim oor;
    oor.wr[0].en = true;
    oor.wr[0].gen_valid = true;
    oor.wr[0].tag = entries;
    oor.wr[0].gen = 1;
    oor.wr[0].data = 0x3333ull;
    const Observed o4 = h->Cycle(oor);
    Require(o4.wr_ctr == 0, "write-admission",
            "a write to a tag with no home entry was admitted");
  }

  // The same offer, correctly routed with a live generation, is admitted and
  // then readable. This is the control that proves the refusals above were not
  // simply "writes never work".
  const uint32_t gen = 9;
  const uint64_t data = 0xa5a5a5a5a5a5a5a5ull;
  Stim good;
  good.wr[sh->bank_of(tag)].en = true;
  good.wr[sh->bank_of(tag)].gen_valid = true;
  good.wr[sh->bank_of(tag)].tag = tag;
  good.wr[sh->bank_of(tag)].gen = gen;
  good.wr[sh->bank_of(tag)].data = data;
  const Observed o5 = h->Cycle(good);
  Require(o5.wr_ctr == 1, "write-admission", "a correctly routed write was refused");

  Stim r2;
  r2.rd[0].valid = true;
  r2.rd[0].tag = tag;
  r2.rd[0].gen = gen;
  const Observed o6 = h->Cycle(r2);
  Require(o6.ready[0] && !o6.never[0] && !o6.mismatch[0] && o6.data[0] == data &&
              o6.gen[0] == gen,
          "write-admission",
          "the admitted write was not readable: got " + Hex64(o6.data[0]) + "/gen " +
              Dec(o6.gen[0]) + " never " + Bool(o6.never[0]));

  reporter->Check(true, "write-admission: only an enabled, live, correctly routed, in-range "
                        "write is admitted");
}

// Phase 7: reset clears validity, and only validity.
void PhaseResetValidity(Harness* h, mosaic::Reporter* reporter, uint32_t entries,
                        uint32_t banks) {
  const ShadowPrf* sh = h->shadow();
  // Write a few entries through their home ports, then read them back.
  std::vector<uint32_t> tags;
  for (uint32_t i = 0; i < std::min<uint32_t>(banks, entries); i++) {
    const uint32_t tag = i * banks;  // distinct banks, one row each
    tags.push_back(tag);
    Stim w;
    const uint32_t port = sh->bank_of(tag);
    w.wr[port].en = true;
    w.wr[port].gen_valid = true;
    w.wr[port].tag = tag;
    w.wr[port].gen = 11;
    w.wr[port].data = 0x1000ull + i;
    h->Cycle(w);
  }
  for (uint32_t tag : tags) {
    Stim r;
    r.rd[0].valid = true;
    r.rd[0].tag = tag;
    r.rd[0].gen = 11;
    const Observed o = h->Cycle(r);
    Require(o.ready[0] && !o.never[0], "reset-validity",
            "entry " + Dec(tag) + " was written but reads as never-written");
  }

  h->Reset(4);

  const std::vector<bool> valid = h->DutValidBits();
  for (uint32_t slot = 0; slot < valid.size(); slot++) {
    Require(!valid[slot], "reset-validity",
            "storage slot " + Dec(slot) + " is still valid after a reset");
  }
  for (uint32_t tag : tags) {
    Stim r;
    r.rd[0].valid = true;
    r.rd[0].tag = tag;
    r.rd[0].gen = 11;
    const Observed o = h->Cycle(r);
    Require(o.never[0], "reset-validity",
            "entry " + Dec(tag) +
                " still reports a value after reset: validity survived the reset");
    Require(!o.mismatch[0], "reset-validity",
            "an entry invalidated by reset reported a generation mismatch");
  }
  reporter->Check(true, "reset-validity: reset clears validity for every entry");
}

// ------------------------------------------------------------ random programme
//
// One seeded programme of writes and demanded reads, with refused demands
// retried out of a queue. The queue's updates are driven by the *pre-edge*
// grants, because those describe the cycle that was presented; updating it from
// anything else would track a different pipeline than the one under test.
uint64_t HashMix(uint64_t hash, uint64_t value) {
  hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
  return hash;
}

struct RandomOutcome {
  uint64_t hash = 0;
  uint64_t bypasses = 0;
  bool drained = false;
};

RandomOutcome RunRandomProgram(Harness* h, uint32_t cycles, uint64_t seed, uint32_t entries,
                               uint32_t tag_w, uint32_t gen_w, uint32_t banks) {
  mosaic::Rng rng(seed);
  std::vector<Demand> pending;
  RandomOutcome out;
  uint64_t hash = 1469598103934665603ull;

  auto random_demand = [&]() {
    Demand d;
    d.valid = true;
    if (((1u << tag_w) > entries) && rng.Chance(5)) {
      d.tag = entries + rng.Below((1u << tag_w) - entries);  // no home entry
    } else {
      d.tag = rng.Below(entries);
    }
    d.gen = rng.Below(1u << gen_w);
    return d;
  };
  auto hash_cycle = [&](const Observed& o) {
    for (uint32_t b = 0; b < banks; b++) {
      uint64_t packed = (o.ready[b] ? 1ull : 0ull) | (o.rsp_valid[b] ? 2ull : 0ull) |
                        (o.mismatch[b] ? 4ull : 0ull) | (o.never[b] ? 8ull : 0ull);
      packed |= static_cast<uint64_t>(o.gen[b]) << 8;
      hash = HashMix(hash, packed);
      hash = HashMix(hash, o.data[b]);
      hash = HashMix(hash, o.tag[b]);
    }
    hash = HashMix(hash, o.wr_ctr);
    hash = HashMix(hash, o.rd_ctr);
    hash = HashMix(hash, o.conflict_ctr);
    hash = HashMix(hash, o.mismatch_ctr);
    hash = HashMix(hash, o.invalid_ctr);
    hash = HashMix(hash, o.busy ? 1u : 0u);
  };

  const uint32_t rows = (entries + banks - 1) / banks;
  for (uint32_t cycle = 0; cycle < cycles; cycle++) {
    Stim s;

    // Writes: enabled most cycles, occasionally on the wrong port, occasionally
    // with a dead generation, occasionally to a tag with no home entry.
    for (uint32_t b = 0; b < banks; b++) {
      if (!rng.Chance(45)) continue;
      WriteOffer& w = s.wr[b];
      w.en = true;
      w.gen_valid = rng.Chance(85);
      const uint32_t roll = rng.Below(100);
      if (banks > 1 && roll < 8) {
        w.tag = (b + 1 + rng.Below(banks - 1)) % banks;  // home bank != b
      } else if (((1u << tag_w) > entries) && roll < 12) {
        w.tag = entries + rng.Below((1u << tag_w) - entries);
      } else {
        w.tag = b + banks * rng.Below(rows);
      }
      w.gen = rng.Below(1u << gen_w);
      w.data = rng.Next();
    }

    // Demand generation, bounded so the queue cannot grow without limit: the
    // register file refuses but never queues, so an unbounded producer would be
    // testing the driver rather than the DUT.
    if (pending.size() < static_cast<size_t>(banks) * 2) {
      for (uint32_t b = 0; b < banks; b++) {
        if (rng.Chance(55)) pending.push_back(random_demand());
      }
    }
    const uint32_t offered =
        std::min<uint32_t>(banks, static_cast<uint32_t>(pending.size()));
    for (uint32_t i = 0; i < offered; i++) s.rd[i] = pending[i];

    out.bypasses += h->shadow()->Eval(s).bypasses;
    const Observed o = h->Cycle(s);

    std::vector<Demand> next;
    next.reserve(pending.size());
    for (size_t i = 0; i < pending.size(); i++) {
      if (i < offered && o.ready[i]) continue;  // served this cycle
      next.push_back(pending[i]);
    }
    pending.swap(next);
    hash_cycle(o);
  }

  // Drain: keep offering the queue until every demand has been served. This is
  // the liveness half of "the consumer re-offers the refused one" -- a register
  // file that dropped a refused demand would leave the queue non-empty forever.
  uint32_t drain_cycles = 0;
  while (!pending.empty() && drain_cycles < banks * 64) {
    Stim s;
    const uint32_t offered =
        std::min<uint32_t>(banks, static_cast<uint32_t>(pending.size()));
    for (uint32_t i = 0; i < offered; i++) s.rd[i] = pending[i];
    const Observed o = h->Cycle(s);
    std::vector<Demand> next;
    next.reserve(pending.size());
    for (size_t i = 0; i < pending.size(); i++) {
      if (i < offered && o.ready[i]) continue;
      next.push_back(pending[i]);
    }
    pending.swap(next);
    drain_cycles++;
    hash_cycle(o);
  }
  out.drained = pending.empty();
  out.hash = hash;
  return out;
}

// Phase 8: the random campaign.
void PhaseRandom(Harness* h, mosaic::Reporter* reporter, uint32_t entries, uint32_t tag_w,
                 uint32_t gen_w, uint32_t banks, uint64_t seed, uint32_t cycles) {
  const RandomOutcome out = RunRandomProgram(h, cycles, seed, entries, tag_w, gen_w, banks);
  Require(out.drained, "random",
          "the demand queue never drained: a refused demand was swallowed instead of being "
          "left for the consumer to re-offer");
  const ShadowPrf* sh = h->shadow();
  // Coverage guard: a green campaign that never conflicted, never read an
  // unwritten entry and never saw a mismatch would be evidence about nothing.
  Require(sh->wr_ctr() > 0, "random", "the campaign admitted no write");
  Require(sh->rd_ctr() > 0, "random", "the campaign granted no read");
  Require(sh->conflict_ctr() > 0, "random",
          "the campaign produced no bank conflict, so the arbitration was never exercised");
  Require(sh->invalid_ctr() > 0, "random", "the campaign read no unwritten entry");
  Require(sh->mismatch_ctr() > 0, "random", "the campaign produced no generation mismatch");
  Require(out.bypasses > 0, "random",
          "the campaign never read an entry in the cycle it was written, so write-through was "
          "never exercised");
  reporter->Check(true, "random: " + Dec(cycles) + " cycles, all comparisons and invariants "
                        "hold");
}

// Phase 9: determinism -- the same seeded programme twice, byte-identical.
void PhaseDeterminism(Harness* h, mosaic::Reporter* reporter, uint32_t entries, uint32_t tag_w,
                      uint32_t gen_w, uint32_t banks, uint64_t seed) {
  const uint32_t cycles = 600;
  const RandomOutcome a = RunRandomProgram(h, cycles, seed, entries, tag_w, gen_w, banks);
  h->Reset(4);
  const Observed cold = h->Cycle(Stim{});
  Require(cold.wr_ctr == 0 && cold.rd_ctr == 0 && cold.conflict_ctr == 0 &&
              cold.mismatch_ctr == 0 && cold.invalid_ctr == 0,
          "determinism", "the counters are not zero after the second reset");
  const RandomOutcome b = RunRandomProgram(h, cycles, seed, entries, tag_w, gen_w, banks);

  Require(a.drained && b.drained, "determinism", "a run did not drain its demand queue");
  Require(a.bypasses == b.bypasses, "determinism",
          "the two runs saw a different number of write-through reads");
  Require(a.hash == b.hash, "determinism",
          "the same seed produced different results: run A hashed " + Hex64(a.hash) +
              ", run B hashed " + Hex64(b.hash));
  reporter->Check(true, "determinism: the same seed reproduces the same run byte for byte");
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
  Vmosaic_prf_tb dut;

  Harness harness(&dut, &clk, options.max_cycles);

  std::string detail;
  bool passed = true;
  try {
    harness.Reset(4);

    // The geometry comes from the elaborated DUT, never from a literal here.
    const uint32_t entries = dut.o_entries;
    const uint32_t tag_w = dut.o_tag_w;
    const uint32_t gen_w = dut.o_gen_w;
    const uint32_t xlen = dut.o_xlen;
    const uint32_t banks = dut.o_banks;
    const uint32_t bank_w = dut.o_bank_w;
    const uint32_t rows = dut.o_rows;
    const uint32_t row_w = dut.o_row_w;

    Require(entries > 0 && tag_w > 0 && gen_w > 0 && xlen > 0 && banks > 0, "geometry",
            "the DUT reported a zero geometry, so the shadow cannot be sized");
    Require(banks <= kMaxBanks, "geometry",
            "the DUT has " + Dec(banks) + " banks, more than this driver can drive (" +
                Dec(kMaxBanks) + ")");
    Require(banks <= entries, "geometry",
            "the DUT has more banks than entries, so some bank holds no entry and the decode "
            "cannot be exercised as documented");
    Require(rows == (entries + banks - 1) / banks, "geometry",
            "the DUT's rows-per-bank (" + Dec(rows) + ") is not ceil(entries/banks) (" +
                Dec((entries + banks - 1) / banks) + ")");
    Require(bank_w == ((banks > 1) ? Clog2(banks) : 1u), "geometry",
            "the DUT's bank decode width (" + Dec(bank_w) + ") does not match its bank count");
    Require(row_w == ((rows > 1) ? Clog2(rows) : 1u), "geometry",
            "the DUT's row decode width (" + Dec(row_w) + ") does not match its row count");
    Require((1u << tag_w) >= entries, "geometry",
            "the " + Dec(tag_w) + "-bit tag field cannot name all " + Dec(entries) + " entries");

    ShadowPrf shadow(banks, entries, tag_w, gen_w, xlen);
    harness.SetGeometry(banks, tag_w, gen_w, xlen);
    auto fresh = [&]() {
      harness.Reset(4);
      shadow.Reset();
      harness.BindShadow(&shadow);
    };

    // Phase order is deliberate. Each phase resets first and owns one mechanism,
    // and a run stops at the first failure, so the order decides *which* phase
    // names a given defect. The directed mechanisms run first and the random
    // soak runs last, because the soak fails on "some cycle" and would mask the
    // phases that can say which structure is at fault.
    fresh();
    harness.Phase("reset-state");
    PhaseResetState(&harness, &reporter, entries, banks);

    fresh();
    harness.Phase("bank-collision");
    PhaseBankCollision(&harness, &reporter, entries, banks);

    fresh();
    harness.Phase("write-through");
    PhaseWriteThrough(&harness, &reporter, entries, banks, xlen);

    fresh();
    harness.Phase("generation");
    PhaseGeneration(&harness, &reporter, xlen);

    fresh();
    harness.Phase("never-written");
    PhaseNeverWritten(&harness, &reporter, entries, banks, tag_w, gen_w);

    fresh();
    harness.Phase("write-admission");
    PhaseWriteAdmission(&harness, &reporter, entries, banks, tag_w);

    fresh();
    harness.Phase("reset-validity");
    PhaseResetValidity(&harness, &reporter, entries, banks);

    fresh();
    harness.Phase("random");
    PhaseRandom(&harness, &reporter, entries, tag_w, gen_w, banks, options.seed, 4000);

    fresh();
    harness.Phase("determinism");
    PhaseDeterminism(&harness, &reporter, entries, tag_w, gen_w, banks, options.seed);

    const ShadowPrf* sh = harness.shadow();
    detail = "prf contract holds: " + std::to_string(harness.comparisons()) +
             " shadow comparisons over " + std::to_string(harness.cycles()) + " cycles, " +
             std::to_string(entries) + " entries / " + std::to_string(banks) + " banks of " +
             std::to_string(rows) + " rows, seed " + std::to_string(options.seed) + "; " +
             std::to_string(sh->wr_ctr()) + " writes, " + std::to_string(sh->rd_ctr()) +
             " granted reads, " + std::to_string(sh->conflict_ctr()) + " conflicts, " +
             std::to_string(sh->mismatch_ctr()) + " mismatches, " +
             std::to_string(sh->invalid_ctr()) + " never-written reads";
  } catch (const Failure& f) {
    reporter.Mismatch(f.what, "contract holds", "contract violated");
    passed = false;
    detail = "contract violated: " + f.what;
  }

  dut.final();
  reporter.Check(passed, "no contract violation in any phase");
  return reporter.Finish(passed ? "PASS" : "FAIL", detail);
}
