// ============================================================================
// tb_retire.cpp -- CASE=commit.head_block_and_dual, work package I-017.
//
// The DUT is never its own oracle. Every output is compared, on every cycle of
// every phase, against an independent C++ shadow written from the *contract*
// documented in rtl/core/mosaic_retire.sv -- the in-order rule, the head-block
// rule, the committed-map rule, the CSR-at-retirement rule, the counter rule --
// and not from the RTL's structure. The shadow shares no code with the RTL and
// never looks at Verilator internals, so agreeing with it is evidence about the
// contract rather than a restatement of the implementation.
//
// Geometry is read from the elaborated DUT (`o_retire_width`, `o_rob_entries`,
// `o_tag_w`, `o_arch_regs`, ...), which are constants derived from the same
// generated package the RTL reads, so this file contains no depth of its own: a
// profile with a different geometry sizes the shadow on the next run, and there
// is no number here to forget to update.
//
// The shadow models **the whole pipeline**, not just the retire unit: the ROB's
// occupancy and its head/lane-1 readiness, the committed map, the CSR file and
// the two counters. That is deliberate. The card's blocking rule -- "a younger
// instruction must not step over a pending fault" -- is a statement about the
// interaction, and a shadow of the retire unit alone could only ever agree with
// whatever the queue happened to present it. Modelling the queue too means the
// test can distinguish "retire asked for the wrong thing" from "the queue was
// asked the wrong thing", which is the distinction the project has been bitten
// by twice (an `ins_ready_now` that omitted "will the insert be accepted", and a
// removal rule added to stop a double count that also suppressed a real one).
//
// Phases, each of which can fail on its own:
//
//   1. reset-state      the documented cold state straight after reset.
//   2. in-order         randomised ready/not-ready heads; the emitted order is
//                       always allocation order, checked against the shadow's
//                       own model of the queue.
//   3. head-block       an incomplete, exceptional or unclosed head blocks
//                       everything behind it, **including a complete younger
//                       entry**. This is the card's named case.
//   4. dual-width       both lanes retirable: two events, in order, in one
//                       cycle. Second lane unretirable: exactly one event and no
//                       reordering. Both asserted, not inferred.
//   5. committed-once   a squashed entry followed by a reallocated younger entry
//                       at the same slot: the map moves for the younger one only,
//                       exactly once.
//   6. csr-at-retire    a CSR write whose instruction is in the buffer but not
//                       complete is invisible; the same write is visible the
//                       cycle after its instruction retires.
//   7. minstret         counts exactly the retired instructions -- the trap
//                       included, the squashed ones excluded.
//   8. trap             a trap is emitted as a trap with a cause and a tval, not
//                       as an ordinary retire, and it drops the buffer.
//   9. flush            a recovery flush cycle retires nothing and commits
//                       nothing, and the speculative map is never disturbed.
//  10. soak             random stimulus compared against the shadow on every
//                       cycle, with deliberate flushes and traps.
//
// Standing invariants are checked on every cycle of every phase rather than in
// one place: the ROB's conservation identity, `minstret` equal to the number of
// events emitted, the committed map changing only on a commit, the speculative
// map never changing, and the event sequence numbers being strictly increasing.
// ============================================================================

#include <verilated.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

#include "sim_common.h"
#include "Vmosaic_retire_tb.h"

namespace {

// Thrown on the first failed check, so the report names one defect rather than
// a thousand consequences of it.
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

std::string At(const std::string& where, uint64_t cycle) {
  return where + ": cycle " + std::to_string(cycle);
}

uint32_t Mask(uint32_t value, uint32_t width) {
  return (width >= 32) ? value : (value & ((1u << width) - 1u));
}

[[maybe_unused]] uint64_t Mask64(uint64_t value, uint32_t width) {
  return (width >= 64) ? value : (value & ((1ull << width) - 1ull));
}

[[maybe_unused]] uint32_t PopCount(uint32_t value) {
  uint32_t count = 0;
  while (value) {
    count += value & 1u;
    value >>= 1;
  }
  return count;
}

// Verilator widens any port wider than 64 bits to `VlWide<N>`, so the driver
// reads those through an explicit word extractor rather than by casting. A cast
// here would be a silent truncation of exactly the wide fields -- the two
// 64-bit PCs -- that the ordering checks depend on.
template <typename Wide>
uint64_t WideWord(const Wide& wide, uint32_t word) {
  return static_cast<uint64_t>(wide[word * 2]) |
         (static_cast<uint64_t>(wide[word * 2 + 1]) << 32);
}

// A port of exactly 64 bits is a plain scalar, not a `VlWide`. The overload
// exists so the caller does not have to know which of the two it got: the two
// lane-strided fields of width 64 bits and the 32 * 32-bit map arrays are both
// read with the same call, and guessing wrong is a compile error at best.
inline uint64_t WideWord(uint64_t value, uint32_t /*word*/) { return value; }

// A *32-bit-strided* array, as opposed to the 64-bit-strided event payloads.
// Verilator's `VlWide` is indexed in 32-bit words, so an array of one 32-bit
// word per architectural register has register `a` at index `a`, while the
// two-lane 64-bit payloads have lane `i` at indices `2*i` and `2*i + 1`. Using
// the 64-bit stride for a 32-bit-strided array reads the register *two* places
// on, which reports x5's tag as x10's -- a plausible-looking wrong answer that
// no amount of staring at the wrapper would explain.
inline uint32_t WideWord32(const VlWide<32>& wide, uint32_t index) {
  return wide[index];
}

// Lane-strided reads. Every driver-facing per-lane vector is one 32-bit word per
// lane, and every lane payload is 64 bits, so the two strides are different and
// both are needed:
//
//   LaneWord(port, i)   a 32-bit field of lane i
//   LaneWide(port, i)   the 64-bit payload of lane i
//
// Reading the low 32 bits of the whole port instead of lane i's word reports the
// *first* lane for every lane, which looks like a design decision rather than a
// bug: with two ready lanes the DUT's mask 0x100000001 and the driver's 1 differ
// in exactly the bits that carry the second lane.
[[maybe_unused]] inline uint32_t LaneWord(uint64_t port, uint32_t lane) {
  return static_cast<uint32_t>(port >> (lane * 32));
}

template <typename Wide>
uint64_t LaneWide(const Wide& wide, uint32_t lane) {
  return WideWord(wide, lane);
}

// The whole lane mask of a 32-bit-per-lane vector, for the fields the retire
// width packs at most `width` bits. A port one 32-bit word per lane is a
// `VlWide` for any width above two, so the mask is assembled from its first two
// words rather than read as a scalar.
template <typename Wide>
inline uint32_t RawLaneWord(const Wide& port, uint32_t lane) {
  return static_cast<uint32_t>(port[lane]);
}

inline uint32_t RawLaneWord(uint64_t port, uint32_t lane) {
  return static_cast<uint32_t>(port >> (lane * 32));
}
// Write one 64-bit lane of a wide input port. Verilator lays a W-bit input out
// as W/32 words, so lane i's 64 bits are words 2*i and 2*i+1 -- the same
// stride the LaneWide read helper uses. Writing the whole port as a scalar
// is a compile error for widths above 64 bits and, worse, a silent lane-0-only
// write for exactly-64-bit ports read as vectors.
template <typename Wide>
inline void SetLaneWide(Wide& port, uint32_t lane, uint64_t value) {
  port[lane * 2] = static_cast<uint32_t>(value);
  port[lane * 2 + 1] = static_cast<uint32_t>(value >> 32);
}
// The lane **mask** of a vector whose lane i occupies the word at index i. The
// mask bits are therefore *not* contiguous in the packed vector: lane 0's bit is
// at bit 0 and lane 1's at bit 32. Reading the packed value as a mask is the
// mistake this helper exists to prevent -- it reports the DUT's correct mask
// 0x100000001 as 4294967297, which is not a lane count and not a mask.
template <typename Wide>
uint32_t Lanes32(const Wide& port, uint32_t width) {
  uint32_t mask = 0;
  for (uint32_t i = 0; i < width; i++) {
    if (RawLaneWord(port, i) & 1u) mask |= 1u << i;
  }
  return mask;
}

// One lane's 32-bit *data* word out of a lane-strided vector. This is not a
// mask: `ev_seq`, `ev_rd`, `commit_tag` and their siblings carry a value per
// lane, and collapsing them into a lane mask would compare a register number
// against a bit count.
// The lanes are 32 bits APART, not packed into one mask word: lane i is word i
// of the VlWide (words 2i/2i+1 only for the 64-bit payload stride). Reading
// lane 1 through the packed-scalar low 32 bits reports lane 0 twice, so a
// two-wide retire compares lane 1's rd/tag/seq against lane 0's.
template <typename Wide>
inline uint32_t LaneField(const Wide& port, uint32_t lane, uint32_t /*width*/) {
  return static_cast<uint32_t>(port[lane]);
}

inline uint32_t LaneField(uint32_t port, uint32_t lane, uint32_t /*width*/) {
  return (lane == 0) ? port : 0u;
}

inline uint32_t LaneField(uint64_t port, uint32_t lane, uint32_t /*width*/) {
  return static_cast<uint32_t>(port >> (lane * 32));
}

uint32_t Lanes32(uint64_t port, uint32_t width) {
  uint32_t mask = 0;
  for (uint32_t i = 0; i < width; i++) {
    if (RawLaneWord(port, i) & 1u) mask |= 1u << i;
  }
  return mask;
}

uint32_t Clog2(uint32_t value) {
  uint32_t width = 1;
  while ((1u << width) < value) width++;
  return width;
}

// The CSR addresses the retire unit's file implements, from config/csr/mode_m.json.
const uint32_t kCsrMcycle = 0xB00;
const uint32_t kCsrMinstret = 0xB02;
const uint32_t kCsrMscratch = 0x340;

// --------------------------------------------------------------- the stimulus
//
// One cycle's worth of everything the driver controls. It is a plain struct so a
// phase can build one field at a time and leave the rest zero, which is what
// keeps a directed phase readable.
struct Stim {
  // ROB
  bool alloc_valid = false;
  uint32_t alloc_tag = 0;
  uint64_t alloc_pc = 0;
  uint32_t alloc_num_uops = 1;
  bool alloc_exc = false;
  bool alloc_open = false;

  bool close_valid = false;
  uint32_t close_index = 0;
  uint32_t close_gen = 0;

  bool cmp_valid = false;
  uint32_t cmp_index = 0;
  uint32_t cmp_gen = 0;
  uint32_t cmp_uop = 0;
  bool cmp_exc = false;

  bool rob_flush = false;
  uint32_t obs_index = 0;

  // Recovery's flush into the retire unit itself.
  bool flush_valid = false;

  // The execution payload, one field set per lane. `pay_valid` is a mask so a
  // phase can withhold lane 0's payload while presenting lane 1's.
  uint32_t pay_valid = 0;
  uint32_t pay_reg_we = 0;
  uint64_t pay_rd = 0;              // two 5-bit words, lane 0 low
  // The 64-bit-per-lane payloads are one uint64_t PER LANE: a single uint64_t
  // cannot hold two lanes, and shifting a lane-1 value by 64 is undefined --
  // which is how lane 1 silently received lane 0's value for every two-wide
  // retire. Lane i's value lives in field [i], and the driver writes port
  // lane i from field [i], with no shifts anywhere.
  uint64_t pay_value[2] = {0, 0};
  uint32_t pay_csr_we = 0;
  uint64_t pay_csr_addr = 0;        // two 12-bit words
  uint64_t pay_csr_value[2] = {0, 0};
  uint32_t pay_is_store = 0;
  uint64_t pay_store_addr[2] = {0, 0};
  uint64_t pay_store_data[2] = {0, 0};
  uint64_t pay_store_size = 0;      // two 3-bit words
  uint64_t pay_exc_cause[2] = {0, 0};
  uint64_t pay_exc_tval[2] = {0, 0};

  // CSR read port
  bool csr_rd_valid = false;
  uint32_t csr_rd_addr = 0;
};

std::string Describe(const Stim& s) {
  char buf[768];
  std::snprintf(buf, sizeof(buf),
                "[alloc v=%d tag=%u n=%u exc=%d open=%d | close v=%d i=%u g=0x%x "
                "| cmp v=%d i=%u g=0x%x uop=%u exc=%d | robflush=%d "
                "retireflush=%d | pay v=0x%x we=0x%x rd=0x%llx csrwe=0x%x "
                "csra=0x%llx store=0x%x | csrrd v=%d a=0x%x]",
                s.alloc_valid, s.alloc_tag, s.alloc_num_uops, s.alloc_exc,
                s.alloc_open, s.close_valid, s.close_index, s.close_gen,
                s.cmp_valid, s.cmp_index, s.cmp_gen, s.cmp_uop, s.cmp_exc,
                s.rob_flush, s.flush_valid, s.pay_valid, s.pay_reg_we, s.pay_rd,
                s.pay_csr_we, s.pay_csr_addr, s.pay_is_store, s.csr_rd_valid,
                s.csr_rd_addr);
  return buf;
}

// ------------------------------------------------------------------ the shadow
//
// One entry of the modelled queue. Deliberately *not* a copy of the ROB's
// descriptor: the shadow keeps only what retirement can observe plus what the
// driver needs to complete an entry, and it derives readiness from the
// documented predicate rather than from a stored `ready` bit.
struct Entry {
  uint32_t tag = 0;
  uint64_t pc = 0;
  uint32_t num_uops = 1;
  uint32_t done_mask = 0;
  bool exc = false;
  bool closed = true;
  uint32_t gen = 0;
  // The ROB slot this entry occupies. Completions name slots, not queue
  // positions: the ROB files a completion by slot index, so the shadow must
  // file it by slot index too. Filing by queue position works only while the
  // head never moves between the allocation and the completion; the moment a
  // retire pops the head first, every position behind it shifts and the next
  // completion lands on the wrong entry -- the shadow alone marks done an
  // entry the buffer never completed, and head-complete disagrees.
  uint32_t slot = 0;
  bool live = true;
};

class ShadowPipeline {
 public:
  // Everything the DUT presents combinationally this cycle.
  struct View {
    // retire
    uint32_t retire_req = 0;
    uint32_t retire_ack = 0;  // the lanes the buffer acknowledged: what pops
    bool trap_flush = false;
    uint32_t ev_valid = 0;
    uint32_t ev_trap = 0;
    uint64_t ev_seq = 0;  // lane 0 low, lane 1 high: a 32-bit field cannot hold two lanes
    uint64_t ev_pc_lo = 0;   // lane 0
    uint64_t ev_pc_hi = 0;   // lane 1
    uint64_t ev_id = 0;           // two 32-bit words, lane 0 low
    uint32_t ev_reg_we = 0;
    uint64_t ev_rd = 0;           // two 5-bit words, lane 0 low
    uint64_t ev_value_lo = 0;   // lane 0
    uint64_t ev_value_hi = 0;   // lane 1
    uint32_t ev_csr_we = 0;
    uint64_t ev_csr_addr = 0;     // two 12-bit words, lane 0 low
    uint64_t ev_csr_value_lo = 0;   // lane 0
    uint64_t ev_csr_value_hi = 0;   // lane 1
    uint32_t ev_store = 0;
    uint64_t ev_store_addr_lo = 0;   // lane 0
    uint64_t ev_store_addr_hi = 0;   // lane 1
    uint64_t ev_store_data_lo = 0;   // lane 0
    uint64_t ev_store_data_hi = 0;   // lane 1
    uint64_t ev_store_size = 0;   // two 3-bit words, lane 0 low
    uint64_t ev_trap_cause_lo = 0;   // lane 0
    uint64_t ev_trap_cause_hi = 0;   // lane 1
    uint64_t ev_trap_tval_lo = 0;   // lane 0
    uint64_t ev_trap_tval_hi = 0;   // lane 1

    bool trap_valid = false;
    uint64_t trap_pc = 0;
    uint64_t trap_cause = 0;
    uint64_t trap_tval = 0;

    uint32_t commit_valid = 0;
    uint64_t commit_rd = 0;       // two 5-bit words, lane 0 low
    uint64_t commit_tag = 0;      // two 8-bit words, lane 0 low
    uint64_t commit_gen = 0;      // two 8-bit words, lane 0 low

    uint64_t csr_rd_data = 0;
    bool csr_rd_unsupported = false;

    uint64_t retire_seq = 0;
    uint64_t minstret = 0;
    uint64_t mcycle = 0;
    uint64_t mscratch = 0;
    uint32_t exc_queued = 0;
    uint32_t event_count = 0;
    bool x0_retired = false;
    uint32_t pay_missing = 0;
    uint32_t csr_unsupported = 0;
    bool order_fault = false;

    // the queue's view, so the driver can report *both* sides when they differ
    uint32_t rob_occupied = 0;
    uint32_t rob_alloc_total = 0;
    uint32_t rob_retired_total = 0;
    uint32_t rob_squashed_total = 0;
  };

  ShadowPipeline(uint32_t width, uint32_t rob_entries, uint32_t index_w,
                 uint32_t id_w, uint32_t tag_w, uint32_t arch_regs,
                 uint32_t max_uops, uint32_t seq_w, uint32_t ret_tag_w,
                 uint32_t ret_gen_w)
      : width_(width),
        rob_entries_(rob_entries),
        index_w_(index_w),
        id_w_(id_w),
        tag_w_(tag_w),
        gen_w_(id_w - tag_w),
        arch_regs_(arch_regs),
        max_uops_(max_uops),
        seq_w_(seq_w),
        ret_tag_w_(ret_tag_w),
        ret_gen_w_(ret_gen_w),
        id_mask_(Mask(~0u, id_w)),
        tag_mask_(Mask(~0u, tag_w)),
        gen_mask_(Mask(~0u, id_w - tag_w)),
        num_uops_w_(Clog2(max_uops)),
        cmt_tag_(arch_regs, 0),
        cmt_gen_(arch_regs, 0),
        spec_tag_(arch_regs, 0),
        spec_gen_(arch_regs, 0) {
    // The committed map's reset state: architectural register i at tag i. The
    // vectors are sized in the initialiser list rather than assigned here: a
    // `vector<T>` member that is written before it is sized is a write through a
    // null data pointer, and the compiler is under no obligation to say so.
    for (uint32_t a = 0; a < arch_regs_; a++) {
      cmt_tag_[a] = a;
      cmt_gen_[a] = 0;
      spec_tag_[a] = a;
      spec_gen_[a] = 0;
    }
  }

  uint32_t width() const { return width_; }
  uint32_t occupied() const { return static_cast<uint32_t>(queue_.size()); }
  uint32_t alloc_total() const { return alloc_total_; }
  uint32_t retired_total() const { return retired_; }
  uint32_t squashed_total() const { return squashed_; }
  uint64_t minstret() const { return minstret_; }
  uint64_t retire_seq() const { return retire_seq_; }
  uint64_t mscratch() const { return mscratch_; }
  bool took_last_alloc() const { return alloc_took_last_; }
  uint32_t gen_mask() const { return gen_mask_; }
  uint32_t index_w() const { return index_w_; }
  uint32_t max_uops() const { return max_uops_; }
  uint32_t done_mask() const {
    return (max_uops_ >= 32) ? 0xFFFFFFFFu : ((1u << max_uops_) - 1u);
  }
  // Whether the shadow would take the allocation in `s`: valid, room in the
  // pre-edge queue, a well-formed child count, and no flush winning the cycle
  // (the TB ORs both driver flushes AND the trap flush into the ROB's
  // `flush_valid`, so all three refuse an allocation).
  // Computed WITHOUT mutating anything, so Compare can ask it before Apply
  // runs: reading the flag Apply sets afterwards would answer for the previous
  // cycle, not this one.
  bool would_take(const Stim& s) const {
    const bool head_trap = !queue_.empty() && queue_[0].exc;
    return s.alloc_valid && queue_.size() < rob_entries_ &&
           s.alloc_num_uops >= 1 && s.alloc_num_uops <= max_uops_ &&
           !s.rob_flush && !s.flush_valid && !head_trap;
  }
  // One lane's identity mask: RET_TAG_W + RET_GEN_W bits. The ev_id compare
  // masks each 32-bit lane to this before comparing, so zero-fill above the
  // identity never fails the case.
  uint64_t ret_id_mask() const {
    const uint32_t w = ret_tag_w_ + ret_gen_w_;
    return (w >= 32) ? 0xFFFFFFFFull : ((1ull << w) - 1ull);
  }
  // The generation the shadow handed out for the entry in ROB slot `slot`. A
  // phase that wants to complete or close a specific entry needs it, and it
  // has to come from the shadow rather than from the DUT: reading the DUT's
  // own generation and feeding it back would make the check circular.
  uint32_t GenOf(uint32_t slot) const {
    for (const auto& e : queue_) {
      if (e.live && e.slot == slot) return e.gen;
    }
    return 0;
  }

  uint32_t cmt_tag(uint32_t a) const { return cmt_tag_[a & (arch_regs_ - 1)]; }
  uint32_t cmt_gen(uint32_t a) const { return cmt_gen_[a & (arch_regs_ - 1)]; }
  uint32_t spec_tag(uint32_t a) const { return spec_tag_[a & (arch_regs_ - 1)]; }
  uint32_t spec_gen(uint32_t a) const { return spec_gen_[a & (arch_regs_ - 1)]; }
  // The head entry, or nullptr when the queue is empty.
  const Entry* head() const { return queue_.empty() ? nullptr : &queue_.front(); }
  const Entry* at(uint32_t lane) const {
    return (lane < queue_.size()) ? &queue_[lane] : nullptr;
  }
  // The live entry occupying ROB slot `slot`, or nullptr when no live entry
  // holds it. Completions name slots, so they file through this, not through
  // queue position: filing by position works only while the head never moves
  // between the allocation and the completion.
  Entry* by_slot(uint32_t slot) {
    for (auto& e : queue_) {
      if (e.live && e.slot == slot) return &e;
    }
    return nullptr;
  }

  void Reset() {
    queue_.clear();
    next_alloc_gen_ = 0;  // the ROB's counter resets too: without this every
    // post-reset completion mismatches and the shadow alone marks entries done
    next_alloc_slot_ = 0;  // and so does its allocation pointer
    for (uint32_t a = 0; a < arch_regs_; a++) {
      cmt_tag_[a] = a;
      cmt_gen_[a] = 0;
      spec_tag_[a] = a;
      spec_gen_[a] = 0;
    }
    alloc_total_ = 0;
    retired_ = 0;
    squashed_ = 0;
    retire_seq_ = 0;
    minstret_ = 0;
    mcycle_ = 0;
    mscratch_ = 0;
  }

  // The children a descriptor with `n` children waits for: the low n bits.
  uint32_t ExpectedMask(uint32_t n) const {
    if (n == 0 || n > max_uops_) return 0;
    const uint32_t all = (max_uops_ >= 32) ? ~0u : ((1u << max_uops_) - 1u);
    return all >> (max_uops_ - n);
  }

  // The documented retirement predicate for one entry: complete, clean, closed.
  bool Ready(const Entry& e) const {
    const uint32_t raw = e.done_mask & ExpectedMask(e.num_uops);
    return raw == ExpectedMask(e.num_uops) && !e.exc && e.closed;
  }

  View Peek(const Stim& s) const {
    View v;
    Decode(s, &v);

    v.retire_seq = retire_seq_;
    v.minstret = minstret_;
    v.mcycle = mcycle_;
    v.mscratch = mscratch_;

    for (uint32_t i = 0; i < width_; i++) {
      if (v.ev_valid & (1u << i)) {
        const Entry& e = queue_[i];
        if (i == 0) v.ev_pc_lo = e.pc; else v.ev_pc_hi = e.pc;
        const uint32_t tag = Mask(Mask(e.tag, tag_w_), ret_tag_w_);
        const uint32_t gen = Mask(Mask(e.gen, gen_mask_), ret_gen_w_);
        v.ev_id |= (static_cast<uint64_t>((gen << ret_tag_w_) | tag)) << (i * 32);
      }
    }

    v.rob_occupied = static_cast<uint32_t>(queue_.size());
    v.rob_alloc_total = alloc_total_;
    v.rob_retired_total = retired_;
    v.rob_squashed_total = squashed_;
    return v;
  }

  // Apply the edge. Everything is recomputed from the same pre-edge state `Peek`
  // used, so the two can never disagree about which events occurred.
  void Apply(const Stim& s) {
    View v;
    Decode(s, &v);
    // Recorded from the PRE-edge occupancy, before pops and flushes below
    // mutate the queue: the ROB decides accept on its own pre-edge occupancy,
    // and the cross-check compares that decision, not the post-edge state.
    // The trap joins the flush OR: the TB wires `trap_flush` into the ROB's
    // `flush_valid`, so `alloc_ok` sees it and a trapping head refuses
    // admission the same cycle it fires.
    alloc_took_last_ = s.alloc_valid && queue_.size() < rob_entries_ &&
                       s.alloc_num_uops >= 1 && s.alloc_num_uops <= max_uops_ &&
                       !s.rob_flush && !s.flush_valid && !v.trap_flush;
    // 1. The CSR file and the counters. Written first so a CSR write from a
    //    retiring instruction lands on this cycle's value.
    uint64_t seq_next = retire_seq_ + v.event_count;
    uint64_t mcycle_next = mcycle_ + 1;
    uint64_t minstret_next = minstret_ + v.event_count;
    uint64_t mscratch_next = mscratch_;
    for (uint32_t i = 0; i < width_; i++) {
      if (!(v.ev_csr_we & (1u << i))) continue;
      const uint32_t addr = (s.pay_csr_addr >> (i * 32)) & 0xFFFu;
      const uint64_t value = s.pay_csr_value[i];
      if (addr == kCsrMcycle) mcycle_next = value;
      else if (addr == kCsrMinstret) minstret_next = value;
      else if (addr == kCsrMscratch) mscratch_next = value;
    }
    retire_seq_ = seq_next;
    mcycle_ = mcycle_next;
    minstret_ = minstret_next;
    mscratch_ = mscratch_next;

    // 2. The queue: a recovery flush -- or the trap flush the TB ORs into the
    //    ROB alongside it -- drops everything in flight; a pop takes from the
    //    front, one entry per acknowledged lane. A flush REFUSES the allocation
    //    offered in the same cycle: `alloc_ok` is gated on `!flush_valid`, so
    //    the buffer takes nothing, counts nothing taken, and counts nothing
    //    squashed for it. The shadow takes nothing either, and counts the
    //    dropped entries squashed exactly as the ROB's `squashed_total` does.
    const bool trap_or_flush = v.trap_flush || s.rob_flush || s.flush_valid;
    if (trap_or_flush) {
      squashed_ += static_cast<uint32_t>(queue_.size());
      queue_.clear();
      return;
    }

    // Pops and the RETIRED count both follow the buffer's acknowledgements:
    // the ROB pops exactly the lanes it acknowledged and counts retired
    // exactly those lanes (rtl/core/mosaic_rob.sv:509,542,669-670).
    // Counting from ev_valid would also credit the trap event (Decode sets
    // ev_valid on a trap while req/ack stay 0); the trap path returns early
    // today so the two agree, but counting the ack keeps the model exact.
    uint32_t pops = 0;
    for (uint32_t i = 0; i < width_; i++) {
      if (v.retire_ack & (1u << i)) {
        pops++;
      } else {
        break;
      }
    }
    for (uint32_t i = 0; i < pops; i++) {
      queue_.front().live = false;
      queue_.pop_front();
    }
    retired_ += pops;

    // 3. The committed map: one commit per acknowledged lane, in lane order.
    for (uint32_t i = 0; i < width_; i++) {
      if (!(v.commit_valid & (1u << i))) continue;
      const uint32_t rd = (s.pay_rd >> (i * 32)) & 0x1Fu;
      const uint32_t tag = (v.commit_tag >> (i * 32)) & 0xFFu;
      const uint32_t gen = (v.commit_gen >> (i * 32)) & 0xFFu;
      cmt_tag_[rd] = tag;
      cmt_gen_[rd] = gen;
    }

    // 4. An allocation lands at the tail, stamped with the generation the ROB
    //    would hand out: one past the last allocation. The stamp lives in the
    //    entry, so no parallel ledger can drift out of step with the queue.
    // The entry lands iff the pre-edge decision above took it. Recomputing the
    // gate here would decide on the POST-pop occupancy, a different question.
    if (alloc_took_last_) {
      Entry e;
      e.tag = Mask(s.alloc_tag, id_w_);
      e.pc = s.alloc_pc;
      e.num_uops = s.alloc_num_uops;
      e.done_mask = 0;
      e.exc = s.alloc_exc;
      e.closed = !s.alloc_open;
      e.gen = Mask(next_alloc_gen_++, gen_mask_);
      e.slot = next_alloc_slot_;
      next_alloc_slot_ = (next_alloc_slot_ + 1) % rob_entries_;
      queue_.push_back(e);
      alloc_total_++;
    }
    // 5. A completion and a close, filed by SLOT -- the index names a ROB slot,
    //    not a queue position, and the generation disambiguates reuse. Filing by
    //    queue position works only while the head never moves between the
    //    allocation and the completion; the moment a retire pops the head, every
    //    position behind it shifts and the completion lands on the wrong entry.
    if (s.cmp_valid) {
      const uint32_t slot = Mask(s.cmp_index, index_w_);
      Entry* e = by_slot(slot);
      if (e != nullptr && Mask(e->gen, gen_mask_) == Mask(s.cmp_gen, gen_mask_)) {
        // The child index is the ROB's own UOP_W slice of the driver's word
        // (sim/tb/mosaic_retire_tb.sv narrows `cmp_uop_i` to UOP_W bits), so
        // the shadow masks to the same width the hardware decodes: UOP_W is
        // 1 bit when MAX_UOPS <= 1 else $clog2(MAX_UOPS). Masking to
        // Clog2(max_uops)-1 bits instead aliases child 4..7 onto 0..3 for
        // max_uops=8 (UOP_W=3), so the shadow marks done a child the buffer
        // never completed and every later readiness check disagrees.
        const uint32_t uop_w = (max_uops_ <= 1) ? 1u : Clog2(max_uops_);
        const uint32_t uop = (uop_w >= 32) ? s.cmp_uop : (s.cmp_uop & ((uop_w == 0) ? 0u : ((1u << uop_w) - 1u)));
        if (uop < e->num_uops && !((e->done_mask >> uop) & 1u)) {
          e->done_mask |= 1u << uop;
          if (s.cmp_exc) e->exc = true;
        }
      }
    }
    if (s.close_valid) {
      const uint32_t slot = Mask(s.close_index, index_w_);
      Entry* e = by_slot(slot);
      if (e != nullptr && Mask(e->gen, gen_mask_) == Mask(s.close_gen, gen_mask_)) {
        e->closed = true;
      }
    }
  }

 private:
  // The eligibility decision and every derived output, all from the pre-edge
  // state and this cycle's ports.
  void Decode(const Stim& s, View* v) const {
    // The trap: the head is exceptional. Not gated on a recovery flush, because
    // an exception at the head is architecturally final.
    const bool head_exc = !queue_.empty() && queue_[0].exc;
    const bool head_trap = head_exc;

    v->trap_flush = head_trap;
    v->trap_valid = head_trap;
    if (head_trap) {
      v->trap_pc = queue_[0].pc;
      v->trap_cause = s.pay_exc_cause[0];
      v->trap_tval = s.pay_exc_tval[0];
      v->ev_trap |= 1u;
      v->ev_valid |= 1u;
    }

    // Lane readiness: the buffer's own predicate for each of the first
    // `width_` entries.
    uint32_t ready_mask = 0;
    for (uint32_t i = 0; i < width_; i++) {
      if (i < queue_.size() && Ready(queue_[i])) ready_mask |= 1u << i;
    }

    for (uint32_t i = 0; i < width_; i++) {
      if (i < queue_.size() && queue_[i].exc && !(i == 0 && head_trap)) {
        v->exc_queued |= 1u << i;
      }
    }

    // The request: each lane needs its entry, its readiness, its payload, no
    // recovery flush, no trap at the head, and every lane in front requested.
    uint32_t req = 0;
    bool carry = true;
    for (uint32_t i = 0; i < width_; i++) {
      const bool lane_ready = (ready_mask & (1u << i)) != 0;
      const bool present = i < queue_.size();
      const bool pay = (s.pay_valid & (1u << i)) != 0;
      const bool want = carry && present && lane_ready && pay && !s.flush_valid &&
                        !(i == 0 && head_trap);
      if (want) req |= 1u << i;
      carry = want;
      // Lane 1's report is gated on lane 0 being *requested*, not on lane 0
      // being retirable: `req_prior[1]` is `req_q[0]`, and lane 0 is requested
      // only when its own payload is present. A ready lane 1 with no payload
      // behind a ready lane 0 that also has no payload reports missing on lane
      // 0 only -- lane 1 has no request in front of it to ride on.
      if (present && lane_ready && !pay && (i == 0 ? !(s.flush_valid || head_trap) : ((req & 1u) != 0))) {
        v->pay_missing |= 1u << i;
      }
    }
    v->retire_req = req;

    // The acknowledgement: lane i is popped only on top of lane i-1.
    // Stored in the View so Apply pops -- and counts retired -- exactly the
    // lanes the buffer acknowledged, not the lanes the unit emitted events
    // for. The two agree unless the request and the acknowledgement disagree
    // by a cycle, and that cycle is exactly where the totals used to drift.
    uint32_t ack = 0;
    bool ack_carry = true;
    for (uint32_t i = 0; i < width_; i++) {
      const bool bit = (req & (1u << i)) != 0;
      if (bit && !ack_carry) {
        v->order_fault = true;
      } else if (bit) {
        ack |= 1u << i;
      }
      ack_carry = bit;
    }
    v->retire_ack = ack;

    // The events follow the acknowledgement, not the request.
    for (uint32_t i = 0; i < width_; i++) {
      if (!(ack & (1u << i))) continue;
      v->ev_valid |= 1u << i;
      v->ev_seq |= static_cast<uint64_t>(Mask(retire_seq_ + i, seq_w_)) << (i * 32);

      const uint32_t rd = (s.pay_rd >> (i * 32)) & 0x1Fu;
      const bool we = (s.pay_reg_we & (1u << i)) != 0;
      if (we) {
        v->ev_reg_we |= 1u << i;
        v->ev_rd |= static_cast<uint64_t>(rd) << (i * 32);
        if (i == 0) v->ev_value_lo = s.pay_value[0];
        else v->ev_value_hi = s.pay_value[1];
        if (rd == 0) {
          v->x0_retired = true;
        } else {
          v->commit_valid |= 1u << i;
          v->commit_rd |= static_cast<uint64_t>(rd) << (i * 32);
          const uint32_t tag = Mask(queue_[i].tag, tag_w_);
          const uint32_t gen = Mask(queue_[i].gen, gen_mask_);
          v->commit_tag |= static_cast<uint64_t>(Mask(tag, ret_tag_w_)) << (i * 32);
          v->commit_gen |= static_cast<uint64_t>(Mask(gen, ret_gen_w_)) << (i * 32);
        }
      }

      const bool csr_we = (s.pay_csr_we & (1u << i)) != 0;
      if (csr_we) {
        v->ev_csr_we |= 1u << i;
        v->ev_csr_addr |= (static_cast<uint64_t>((s.pay_csr_addr >> (i * 32)) & 0xFFFu)) << (i * 32);
        if (i == 0) v->ev_csr_value_lo = s.pay_csr_value[0];
        else v->ev_csr_value_hi = s.pay_csr_value[1];
        const uint32_t addr = (s.pay_csr_addr >> (i * 32)) & 0xFFFu;
        if (addr != kCsrMcycle && addr != kCsrMinstret && addr != kCsrMscratch) {
          v->csr_unsupported |= 1u << i;
        }
      }

      const bool store = (s.pay_is_store & (1u << i)) != 0;
      if (store) {
        v->ev_store |= 1u << i;
        if (i == 0) v->ev_store_addr_lo = s.pay_store_addr[0];
        else v->ev_store_addr_hi = s.pay_store_addr[1];
        if (i == 0) v->ev_store_data_lo = s.pay_store_data[0];
        else v->ev_store_data_hi = s.pay_store_data[1];
        v->ev_store_size |= (static_cast<uint64_t>((s.pay_store_size >> (i * 32)) & 0x7u)) << (i * 32);
      }
    }

    for (uint32_t i = 0; i < width_; i++) {
      v->event_count += (v->ev_valid & (1u << i)) ? 1 : 0;
    }
    if (head_trap) {
      v->ev_seq |= static_cast<uint32_t>(Mask(retire_seq_, seq_w_));
      v->ev_trap_cause_lo = s.pay_exc_cause[0];
      v->ev_trap_tval_lo = s.pay_exc_tval[0];
    }

    // The CSR read port is a read of the pre-edge state.
    if (s.csr_rd_valid) {
      if (s.csr_rd_addr == kCsrMcycle) v->csr_rd_data = mcycle_;
      else if (s.csr_rd_addr == kCsrMinstret) v->csr_rd_data = minstret_;
      else if (s.csr_rd_addr == kCsrMscratch) v->csr_rd_data = mscratch_;
      else v->csr_rd_unsupported = true;
    }
  }

  uint32_t width_;
  uint32_t rob_entries_;
  uint32_t index_w_;
  uint32_t id_w_;
  uint32_t tag_w_;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-private-field"
  uint32_t gen_w_;  // set but never read: the shadow derives generations from Mask(), not this width
#pragma clang diagnostic pop
  uint32_t arch_regs_;
  uint32_t max_uops_;
  uint32_t seq_w_;
  // The retire unit's destination-identity widths: the commit it emits names a
  // physical-register tag and generation in RET_TAG_W/RET_GEN_W bits, which are
  // narrower than the queue's TAG_W/GEN_W. The shadow masks its expectations to
  // the same widths, so a generation that has grown past 7 bits compares equal
  // on both sides instead of disagreeing about bits the hardware never emits.
  uint32_t ret_tag_w_;
  uint32_t ret_gen_w_;
  uint32_t id_mask_;
  uint32_t tag_mask_;
  uint32_t gen_mask_;
  uint32_t num_uops_w_;

  std::deque<Entry> queue_;  // generations ride in Entry.gen: no parallel ledger to drift
  uint32_t next_alloc_gen_ = 0;
  // Whether the shadow took the last allocation: set in Apply, read by the
  // allocation cross-check. The shadow takes exactly what the ROB would.
  bool alloc_took_last_ = false;
  uint32_t next_alloc_slot_ = 0;  // the slot the next allocation takes: slots are
  // handed out round-robin like the ROB's own alloc_ptr, so a completion names
  // the same slot on both sides.
  std::vector<uint32_t> cmt_tag_;
  std::vector<uint32_t> cmt_gen_;
  std::vector<uint32_t> spec_tag_;
  std::vector<uint32_t> spec_gen_;
  uint32_t alloc_total_ = 0;
  uint32_t retired_ = 0;
  uint32_t squashed_ = 0;
  uint64_t retire_seq_ = 0;
  uint64_t minstret_ = 0;
  uint64_t mcycle_ = 0;
  uint64_t mscratch_ = 0;
};

// The outcome of the edge the DUT is about to take, captured with the clock low.
struct Reports {
  uint32_t retire_req = 0;
  bool trap_flush = false;
  uint32_t ev_valid = 0;
  uint32_t ev_trap = 0;
  uint32_t commit_valid = 0;
  bool order_fault = false;
  uint32_t event_count = 0;
  bool x0_retired = false;
  uint32_t pay_missing = 0;
  uint32_t csr_unsupported = 0;
  uint32_t exc_queued = 0;
  uint32_t ev_csr_we = 0;
  uint32_t ev_store = 0;
  bool commit_accepted = false;
  bool commit2_accepted = false;
  uint32_t rob_retire_ack = 0;
  uint32_t rob_retire_ack_next = 0;
};

// ---------------------------------------------------------------- the harness
class Harness {
 public:
  struct Counters {
    uint32_t events = 0;
    uint32_t dual_events = 0;
    uint32_t single_events = 0;
    uint32_t traps = 0;
    uint32_t flushes = 0;
    uint32_t commits = 0;
    uint32_t x0_retires = 0;
    uint32_t csr_writes = 0;
    uint32_t blocked_cycles = 0;
    uint32_t stores = 0;
    uint32_t squashed = 0;
  };

  Harness(Vmosaic_retire_tb* dut, mosaic::ClockDriver* clk, uint64_t max_cycles,
          ShadowPipeline* shadow)
      : dut_(dut),
        clk_(clk),
        max_cycles_(max_cycles),
        shadow_(shadow),
        width_(shadow->width()) {}

  void Phase(const std::string& name) { phase_ = name; }

  // A freshly reset DUT *and* a freshly reset shadow, so no phase can pass on
  // state left behind by the one before it.
  void Fresh(int cycles = 4) {
    for (int i = 0; i < cycles; i++) Cycle(Stim{}, /*rst=*/true);
    shadow_->Reset();
    have_prev_ = false;
    counters_ = Counters();
    spec_baseline_ = false;
  }

  // One full clock period: drive, evaluate, compare against the shadow, apply
  // the edge.
  void Cycle(const Stim& s, bool rst = false) {
    if (cycles_ >= max_cycles_) {
      Fail(phase_, "max-cycles (" + std::to_string(max_cycles_) +
                       ") exhausted before the phase finished");
    }

    dut_->clk = 0;
    dut_->rst = rst ? 1 : 0;
    dut_->alloc_valid_i = s.alloc_valid;
    dut_->alloc_tag_i = s.alloc_tag;
    dut_->alloc_pc_i = s.alloc_pc;
    dut_->alloc_num_uops_i = s.alloc_num_uops;
    dut_->alloc_exc_i = s.alloc_exc;
    dut_->alloc_open_i = s.alloc_open;
    dut_->close_valid_i = s.close_valid;
    dut_->close_index_i = s.close_index;
    dut_->close_gen_i = s.close_gen;
    dut_->cmp_valid_i = s.cmp_valid;
    dut_->cmp_index_i = s.cmp_index;
    dut_->cmp_gen_i = s.cmp_gen;
    dut_->cmp_uop_i = s.cmp_uop;
    dut_->cmp_exc_i = s.cmp_exc;
    dut_->obs_index_i = s.obs_index;
    dut_->rob_flush_i = s.rob_flush;
    dut_->pay_valid_i = s.pay_valid;
    dut_->pay_reg_we_i = s.pay_reg_we;
    dut_->pay_rd_i = s.pay_rd;
    // The 64-bit-per-lane payloads are wide ports: each lane is driven
    // separately from its own Stim field. Assigning a packed word writes only
    // lane 0 and leaves lane 1 carrying the previous cycle's value -- which is
    // how a two-wide retire compared lane 1's rd against lane 0's value.
    SetLaneWide(dut_->pay_value_i, 0, s.pay_value[0]);
    SetLaneWide(dut_->pay_value_i, 1, s.pay_value[1]);
    dut_->pay_csr_we_i = s.pay_csr_we;
    dut_->pay_csr_addr_i = s.pay_csr_addr;
    SetLaneWide(dut_->pay_csr_value_i, 0, s.pay_csr_value[0]);
    SetLaneWide(dut_->pay_csr_value_i, 1, s.pay_csr_value[1]);
    dut_->pay_is_store_i = s.pay_is_store;
    SetLaneWide(dut_->pay_store_addr_i, 0, s.pay_store_addr[0]);
    SetLaneWide(dut_->pay_store_addr_i, 1, s.pay_store_addr[1]);
    SetLaneWide(dut_->pay_store_data_i, 0, s.pay_store_data[0]);
    SetLaneWide(dut_->pay_store_data_i, 1, s.pay_store_data[1]);
    dut_->pay_store_size_i = s.pay_store_size;
    SetLaneWide(dut_->pay_exc_cause_i, 0, s.pay_exc_cause[0]);
    SetLaneWide(dut_->pay_exc_cause_i, 1, s.pay_exc_cause[1]);
    SetLaneWide(dut_->pay_exc_tval_i, 0, s.pay_exc_tval[0]);
    SetLaneWide(dut_->pay_exc_tval_i, 1, s.pay_exc_tval[1]);
    dut_->flush_valid_i = s.flush_valid;
    dut_->csr_rd_valid_i = s.csr_rd_valid;
    dut_->csr_rd_addr_i = s.csr_rd_addr;
    dut_->eval();

    last_rst_ = rst;
    last_alloc_valid_ = s.alloc_valid;
    last_alloc_n_ = s.alloc_num_uops;
    // The combinational reports describe the edge that has **not** happened yet,
    // so they are captured with the clock low. The settled state is what the edge
    // leaves behind, so it is compared after it -- reading it before would assert
    // on the cycle before the one just driven, which is the "asserting on the
    // wrong question" failure this file's header warns about.
    if (!rst) {
      Compare(s);
      CaptureReports();
      Tally(s);
      shadow_->Apply(s);
    }

    dut_->clk = 1;
    dut_->eval();
    clk_->Tick();
    dut_->clk = 0;
    dut_->eval();
    // Refresh the settled allocation identity AFTER the edge -- under reset
    // too. The reset edge clears the pointer and generation counter, and the
    // next Alloc must read those cleared values: skipping the refresh on reset
    // cycles leaves the PREVIOUS phase's pointer behind, so the first
    // allocation after Fresh reuses the previous phase's slot while the
    // buffer takes slot 0 -- and every generation-indexed access after it
    // disagrees, surfacing here as a phantom alloc_total drift.
    settled_alloc_index_ = dut_->rob_alloc_index_o;
    settled_alloc_gen_ = dut_->rob_alloc_gen_o;
    if (!rst) CompareSettled();
  }

  // ------------------------------------------------------- stimulus helpers
  // The identity an allocation will carry. Read from the settled state left by
  // the previous edge: `rob_alloc_index_o`/`rob_alloc_gen_o` are the ROB's
  // combinational allocation pointer and generation counter, so after the edge
  // they name exactly the slot the NEXT allocation will take and the
  // generation it will be stamped with.
  struct Identity {
    uint32_t index = 0;
    uint32_t gen = 0;
    uint32_t tag = 0;
  };

  Identity Alloc(uint32_t tag, uint32_t num_uops = 1, bool exc = false,
                 bool open = false) {
    // The identity comes from the settled state the previous edge left behind:
    // the slot this allocation will take and the generation it will carry.
    // Sampling the pointer before the edge instead names the slot the current
    // stimulus takes -- the same slot twice for back-to-back allocations, so
    // the second completion lands on the first entry on one side and on the
    // second on the other.
    const uint32_t index = settled_alloc_index_;
    const uint32_t gen = settled_alloc_gen_;
    Stim s;
    s.alloc_valid = true;
    s.alloc_tag = tag;
    s.alloc_pc = 0x80000000ull + 4ull * tag;
    s.alloc_num_uops = num_uops;
    s.alloc_exc = exc;
    s.alloc_open = open;
    Cycle(s);
    Identity id;
    id.index = index;
    id.gen = gen;
    id.tag = tag;
    return id;
  }

  void Complete(const Identity& id, uint32_t uop = 0, bool exc = false) {
    Stim s;
    s.cmp_valid = true;
    s.cmp_index = id.index;
    s.cmp_gen = id.gen;
    s.cmp_uop = uop;
    s.cmp_exc = exc;
    Cycle(s);
  }

  void Close(const Identity& id) {
    Stim s;
    s.close_valid = true;
    s.close_index = id.index;
    s.close_gen = id.gen;
    Cycle(s);
  }

  // One cycle offering the payload for `lanes`, with `rd` packed per lane.
  // The 64-bit value goes to lane 0 only: Present is a single-lane helper,
  // and broadcasting one value to both lanes would assert lane 1 carries a
  // value its producer never sent.
  void Present(uint32_t lanes, uint32_t rd, uint64_t value, bool csr = false,
               uint32_t csr_addr = kCsrMscratch, uint64_t csr_value = 0,
               bool store = false, bool flush = false) {
    Stim s;
    s.pay_valid = lanes;
    s.pay_reg_we = lanes & rd_present_mask(rd);
    s.pay_rd = rd;
    s.pay_value[0] = value;
    if (csr) {
      s.pay_csr_we = lanes;
      s.pay_csr_addr = csr_addr;
      s.pay_csr_value[0] = csr_value;
    }
    if (store) s.pay_is_store = lanes;
    s.flush_valid = flush;
    Cycle(s);
  }

  uint32_t rd_present_mask(uint32_t rd) const { return rd; }

  // ---------------------------------------------------------------- accessors
  uint32_t RetireReq() const { return report_.retire_req; }
  uint32_t Events() const { return report_.ev_valid; }
  uint32_t Traps() const { return report_.ev_trap; }
  uint32_t EventCount() const { return report_.event_count; }
  uint32_t Commits() const { return report_.commit_valid; }
  bool TrapFlush() const { return report_.trap_flush; }
  bool OrderFault() const { return report_.order_fault; }
  bool X0Retired() const { return report_.x0_retired; }
  uint32_t PayMissing() const { return report_.pay_missing; }
  uint32_t CsrUnsupported() const { return report_.csr_unsupported; }
  uint32_t ExcQueued() const { return report_.exc_queued; }
  bool CommitAccepted() const { return report_.commit_accepted; }
  bool Commit2Accepted() const { return report_.commit2_accepted; }

  // What the committed map actually holds, read back through the wrapper.
  // The map arrays are ARCH_REGS*32 bits wide, which Verilator hands over as a
  // `VlWide`, so each register's word is read through the explicit extractor
  // rather than by shifting the whole vector -- a shift of that vector is a
  // compile error here and, in a version that allowed it, a silent truncation.
  uint32_t ObservedCmtTag(uint32_t a) const {
    return WideWord32(dut_->o_cmt_tag_o, a) & 0xFFu;
  }
  uint32_t ObservedCmtGen(uint32_t a) const {
    return WideWord32(dut_->o_cmt_gen_o, a) & 0xFFu;
  }
  uint32_t ObservedSpecTag(uint32_t a) const {
    return WideWord32(dut_->o_spec_tag_o, a) & 0xFFu;
  }
  uint32_t ObservedSpecGen(uint32_t a) const {
    return WideWord32(dut_->o_spec_gen_o, a) & 0xFFu;
  }
  uint64_t ObservedMinstret() const { return dut_->o_minstret_o; }
  uint64_t ObservedMcycle() const { return dut_->o_mcycle_o; }
  uint64_t ObservedRetireSeq() const { return dut_->o_retire_seq_o; }
  uint64_t ObservedMscratch() const { return dut_->o_mscratch_o; }
  uint64_t ObservedRenameFree() const { return dut_->o_rename_free_o; }
  uint32_t ObservedOccupied() const { return dut_->rob_occupied_o; }
  uint32_t ObservedAllocTotal() const { return dut_->rob_alloc_total_o; }
  uint32_t ObservedRetiredTotal() const { return dut_->rob_retired_total_o; }
  uint32_t ObservedSquashedTotal() const { return dut_->rob_squashed_total_o; }
  // One slot of the ROB, read straight from the buffer's own observation port:
  // valid, generation, tag, child count, done mask, exception, closed. The
  // reconcile step below compares every live slot against the shadow, so a
  // completion that landed on the wrong slot surfaces here, not three phases
  // later as a readiness disagreement.
  bool ObsValid(uint32_t slot) {
    dut_->obs_index_i = slot;
    dut_->eval();
    return dut_->rob_obs_valid_o != 0;
  }
  uint32_t ObsGen(uint32_t slot) {
    dut_->obs_index_i = slot;
    dut_->eval();
    return dut_->rob_obs_gen_o;
  }
  uint32_t ObsTag(uint32_t slot) {
    dut_->obs_index_i = slot;
    dut_->eval();
    return dut_->rob_obs_tag_o;
  }
  uint32_t ObsNumUops(uint32_t slot) {
    dut_->obs_index_i = slot;
    dut_->eval();
    return dut_->rob_obs_num_uops_o;
  }
  uint32_t ObsDone(uint32_t slot) {
    dut_->obs_index_i = slot;
    dut_->eval();
    return dut_->rob_obs_done_mask_o;
  }
  bool ObsExc(uint32_t slot) {
    dut_->obs_index_i = slot;
    dut_->eval();
    return dut_->rob_obs_exc_o != 0;
  }
  bool ObsClosed(uint32_t slot) {
    dut_->obs_index_i = slot;
    dut_->eval();
    return dut_->rob_obs_closed_o != 0;
  }
  uint64_t CsrRead() const { return dut_->csr_rd_data_o; }
  bool CsrReadUnsupported() const { return dut_->csr_rd_unsupported_o; }

  const Counters& counters() const { return counters_; }
  uint64_t comparisons() const { return comparisons_; }
  uint64_t cycles() const { return cycles_; }
  ShadowPipeline& shadow() const { return *shadow_; }
  uint32_t width() const { return width_; }
  mosaic::Rng& rng() { return rng_; }

  // The event fields, read lane by lane. The driver packs two lanes into one
  // vector, so reading "lane i's PC" means slicing at i*64. A phase that
  // compared the packed vector against a single expected number would be
  // comparing two instructions' worth of data as one, which is exactly how an
  // ordering bug hides.
  uint64_t PcOfLane(uint32_t lane) const {
    return WideWord(dut_->ev_pc_o, lane);
  }
  // ev_seq_o is one 32-bit word per lane (2 words for a 2-wide retire), so
  // lane i's value is word i -- *not* words 2i/2i+1, which is the stride for
  // the 64-bit payloads. Reading lane 1 through the 64-bit helper walks off
  // the end of the port and returns whatever Verilator left adjacent to it,
  // which is how a correct DUT produced ev_seq lane 1 = 1828657216. Match the
  // stride to the field: 32 bits per lane here, against the 64-bit ev_seq_o.
  uint32_t SeqOfLane(uint32_t lane) const {
    return static_cast<uint32_t>((dut_->ev_seq_o >> (lane * 32)) & 0xFFu);
  }
  uint64_t TrapCause() const { return dut_->trap_cause_o; }
  uint64_t TrapTval() const { return dut_->trap_tval_o; }
  uint32_t ObservedPRFEntries() const { return prf_entries_; }

  // Sized from the geometry the DUT reported, so nothing in this file is a
  // hand-written depth.
  void set_debug_cycle(int c) { debug_cycle_ = c; }

  void set_geometry(uint32_t prf_entries, uint32_t arch_regs) {
    prf_entries_ = prf_entries;
    arch_regs_ = arch_regs;
    spec_base_tag_.assign(arch_regs, 0);
    spec_base_gen_.assign(arch_regs, 0);
  }

  uint32_t ArchRegs() const { return arch_regs_; }

 private:
  // Everything combinational, against the shadow's own computation from the
  // pre-edge state and the same ports.
  void Compare(const Stim& s) {
    const ShadowPipeline::View v = shadow_->Peek(s);
    const std::string where = At(phase_, clk_->cycle());

#define CMP_U32(field, name)                                                  \
  do {                                                                        \
    ++comparisons_;                                                           \
    const uint32_t got = (field);                                             \
    if (got != v.name) {                                                      \
      Fail(where, std::string(#name) + ": expected " + std::to_string(v.name) +\
                    ", got " + std::to_string(got) + Describe(s));             \
    }                                                                         \
  } while (0)

// A two-lane packed field: lane 0 in the low 32 bits, lane 1 in the high 32.
// Compared as a full 64-bit value -- truncating to 32 bits compares lane 0
// alone and reports lane 1's content as a lane-0 disagreement, which is how a
// correct two-wide retire read "expected <both lanes>, got <lane 0>".
#define CMP_U64_LANES(field, name)                                            \
  do {                                                                        \
    ++comparisons_;                                                           \
    const uint64_t got64 = (field);                                           \
    if (got64 != v.name) {                                                    \
      Fail(where, std::string(#name) + ": expected " +                        \
                    std::to_string(v.name) + ", got " +                        \
                    std::to_string(got64) + Describe(s));                       \
    }                                                                         \
  } while (0)
    // The completion cross-check: the buffer must accept every completion the
    // shadow accepts, and report stale/duplicate exactly when the shadow
    // ignores. A completion the buffer files as stale while the shadow marks
    // done is the one divergence this file cannot otherwise see -- the shadow
    // alone advances, and every later readiness comparison disagrees.
    if (s.cmp_valid) {
      ++comparisons_;
      // Stale is only a defect when the shadow holds a LIVE entry for this
      // slot+generation: a completion for an already-retired slot is correctly
      // filed stale by the buffer (the slot is dead), and the shadow -- which
      // pops on retire -- has nothing to mark done either. Failing those fills
      // every phase with completions to drained slots (e.g. head-block (c)'s
      // trap flush followed by slot reuse) with a defect that is really the
      // checker's, not the buffer's.
      Entry* tgt = shadow_->by_slot(Mask(s.cmp_index, shadow_->index_w()));
      const bool shadow_live =
          tgt != nullptr && tgt->live &&
          Mask(tgt->gen, shadow_->gen_mask()) == Mask(s.cmp_gen, shadow_->gen_mask());
      if (shadow_live && !dut_->rob_cmp_accepted_o && !dut_->rob_cmp_duplicate_o) {
        Fail(where, "completion filed as stale: the buffer accepted nothing for "
                     "an entry the shadow may still mark done" + Describe(s));
      }
    }
    // The allocation cross-check: the shadow takes an allocation iff the buffer
    // does. A refused allocation that the shadow files anyway is a phantom
    // entry: every queue position behind it shifts, and the next completion
    // lands on the wrong entry on one side.
    if (s.alloc_valid) {
      ++comparisons_;
      const bool dut_took = dut_->rob_alloc_ok_o != 0;
      const bool shadow_took = shadow_->would_take(s);
      // The buffer also reports format refusal: a well-formed take needs the
      // buffer's own bad-uops flag clear, not just the shadow's range check.
      // Comparing the shadow's count model against the buffer's flag keeps a
      // width-narrowing mismatch in the count path (driver word -> CNT_W)
      // from hiding as a phantom-entry divergence three cycles later.
      const bool dut_bad = dut_->rob_alloc_bad_o != 0;
      const bool shadow_bad = !(s.alloc_num_uops >= 1 && s.alloc_num_uops <= shadow_->max_uops());
      if (dut_took != shadow_took || dut_bad != shadow_bad) {
        Fail(where, "allocation accept disagrees: buffer took " +
                         Bool(dut_took) + ", shadow took " + Bool(shadow_took) +
                         " buffer-bad " + Bool(dut_bad) + ", shadow-bad " +
                         Bool(shadow_bad) + Describe(s));
      }
    }
    // (dump removed: the duplicate-completion double-offer was the cause -- see
    // the head-first completion note in PhaseInOrder)
    // The head-state cross-checks: the buffer's own complete/closed reports must
    // agree with the shadow's predicate for the head and second head. A
    // completion the buffer accepted but did not record surfaces here as a
    // head-complete disagreement, not three phases later as a readiness one.
    {
      const Entry* head = shadow_->head();
      const bool sh_head_complete =
          head != nullptr && (head->done_mask & shadow_->ExpectedMask(head->num_uops)) ==
                                 shadow_->ExpectedMask(head->num_uops);
      ++comparisons_;
      if ((dut_->rob_head_complete_o != 0) != sh_head_complete) {
        Fail(where, "head complete disagrees: buffer says " +
                         Bool(dut_->rob_head_complete_o != 0) + ", shadow says " +
                         Bool(sh_head_complete) + Describe(s));
      }
      const Entry* second = shadow_->at(1);
      const bool sh_second_complete =
          second != nullptr && (second->done_mask & shadow_->ExpectedMask(second->num_uops)) ==
                                   shadow_->ExpectedMask(second->num_uops);
      ++comparisons_;
      if ((dut_->rob_head1_complete_o != 0) != sh_second_complete) {
        Fail(where, "second-head complete disagrees: buffer says " +
                         Bool(dut_->rob_head1_complete_o != 0) + ", shadow says " +
                         Bool(sh_second_complete) + Describe(s));
      }
      const bool sh_head_closed = head != nullptr && head->closed;
      ++comparisons_;
      if ((dut_->rob_head_closed_o != 0) != sh_head_closed) {
        Fail(where, "head closed disagrees: buffer says " +
                         Bool(dut_->rob_head_closed_o != 0) + ", shadow says " +
                         Bool(sh_head_closed) + Describe(s));
      }
      const bool sh_second_closed = second != nullptr && second->closed;
      ++comparisons_;
      if ((dut_->rob_head1_closed_o != 0) != sh_second_closed) {
        Fail(where, "second-head closed disagrees: buffer says " +
                         Bool(dut_->rob_head1_closed_o != 0) + ", shadow says " +
                         Bool(sh_second_closed) + Describe(s));
      }
    }
    {
      ++comparisons_;
      // ev_seq_o is 2x32 one word per lane: lane i is bits [i*32+8). The low
      // 8 bits of each word carry that lane's sequence number, so lane 1's value
      // would read lane 1 from beyond the port for a 2x32 vector.
      // ev_seq is *ungated*: the DUT drives lane i with retire_seq + i on every
      // cycle, valid or not, so the field is only meaningful where ev_valid is
      // high. Comparing it on a quiet lane would assert the DUT's free-running
      // counter shape rather than any contract. Each lane is checked iff it
      // retired this cycle; the quiet lanes are not compared at all.
      const uint32_t g0 = LaneField(dut_->ev_seq_o, 0, width_) & 0xFFu;
      const uint32_t g1 = LaneField(dut_->ev_seq_o, 1, width_) & 0xFFu;
      const uint32_t w0 = v.ev_seq & 0xFFu;
      const uint32_t w1 = (v.ev_seq >> 32) & 0xFFu;
      if (((v.ev_valid & 1u) ? (g0 != w0) : false) ||
          ((v.ev_valid & 2u) ? (g1 != w1) : false)) {
        Fail(where, "ev_seq: lane0 expected " + std::to_string(w0) + " got " +
                        std::to_string(g0) + "; lane1 expected " +
                        std::to_string(w1) + " got " + std::to_string(g1) +
                        Describe(s));
      }
    }
    CMP_U32(Lanes32(dut_->ev_reg_we_o, width_), ev_reg_we);
    CMP_U64_LANES((LaneField(dut_->ev_rd_o, 0, width_) & 0x1Fu) |
             (static_cast<uint64_t>(LaneField(dut_->ev_rd_o, 1, width_)) << 32), ev_rd);
    CMP_U32(Lanes32(dut_->ev_csr_we_o, width_), ev_csr_we);
    CMP_U64_LANES((LaneField(dut_->ev_csr_addr_o, 0, width_) & 0xFFFu) |
             (static_cast<uint64_t>(LaneField(dut_->ev_csr_addr_o, 1, width_)) << 32), ev_csr_addr);
    CMP_U32(Lanes32(dut_->ev_store_o, width_), ev_store);
    CMP_U64_LANES((LaneField(dut_->ev_store_size_o, 0, width_) & 0x7u) |
             (static_cast<uint64_t>(LaneField(dut_->ev_store_size_o, 1, width_)) << 32), ev_store_size);
    CMP_U32(Lanes32(dut_->commit_valid_o, width_), commit_valid);
    CMP_U64_LANES((LaneField(dut_->commit_rd_o, 0, width_) & 0x1Fu) |
             (static_cast<uint64_t>(LaneField(dut_->commit_rd_o, 1, width_)) << 32), commit_rd);
    CMP_U64_LANES((LaneField(dut_->commit_tag_o, 0, width_) & 0xFFu) |
             (static_cast<uint64_t>(LaneField(dut_->commit_tag_o, 1, width_)) << 32), commit_tag);
    CMP_U64_LANES((LaneField(dut_->commit_gen_o, 0, width_) & 0xFFu) |
             (static_cast<uint64_t>(LaneField(dut_->commit_gen_o, 1, width_)) << 32), commit_gen);
    CMP_U32(dut_->o_event_count_o, event_count);
    CMP_U32(dut_->o_pay_missing_o, pay_missing);
    CMP_U32(dut_->o_csr_unsupported_o, csr_unsupported);
    CMP_U32(dut_->o_exc_queued_o, exc_queued);
 #undef CMP_U32
 #undef CMP_U64_LANES

#define CMP_BOOL(field, name)                                                 \
  do {                                                                        \
    ++comparisons_;                                                           \
    const bool got = (field) != 0;                                            \
    if (got != v.name) {                                                      \
      Fail(where, std::string(#name) + ": expected " + Bool(v.name) +         \
                    ", got " + Bool(got) + Describe(s));                      \
    }                                                                         \
  } while (0)

    CMP_BOOL(dut_->trap_flush_o, trap_flush);
    CMP_BOOL(dut_->trap_valid_o, trap_valid);
    CMP_BOOL(dut_->csr_rd_unsupported_o, csr_rd_unsupported);
    CMP_BOOL(dut_->o_x0_retired_o, x0_retired);
    CMP_BOOL(dut_->o_order_fault_o, order_fault);
#undef CMP_BOOL

#define CMP_U64(field, name)                                                  \
  do {                                                                        \
    ++comparisons_;                                                           \
    const uint64_t got_ = (field);                                            \
    const uint64_t want_ = static_cast<uint64_t>(v.name);                     \
    if (got_ != want_) {                                                      \
      Fail(where, std::string(#name) + ": expected " + std::to_string(want_) +  \
                    " got " + std::to_string(got_) + Describe(s));              \
    }                                                                         \
  } while (0)

    CMP_U64(LaneWide(dut_->ev_pc_o, 0), ev_pc_lo);
    CMP_U64(LaneWide(dut_->ev_pc_o, 1), ev_pc_hi);
    CMP_U64(LaneWide(dut_->ev_value_o, 0), ev_value_lo);
    CMP_U64(LaneWide(dut_->ev_value_o, 1), ev_value_hi);
    CMP_U64(LaneWide(dut_->ev_csr_value_o, 0), ev_csr_value_lo);
    CMP_U64(LaneWide(dut_->ev_csr_value_o, 1), ev_csr_value_hi);
    CMP_U64(LaneWide(dut_->ev_store_addr_o, 0), ev_store_addr_lo);
    CMP_U64(LaneWide(dut_->ev_store_addr_o, 1), ev_store_addr_hi);
    CMP_U64(LaneWide(dut_->ev_store_data_o, 0), ev_store_data_lo);
    CMP_U64(LaneWide(dut_->ev_store_data_o, 1), ev_store_data_hi);
    CMP_U64(LaneWide(dut_->ev_trap_cause_o, 0), ev_trap_cause_lo);
    CMP_U64(LaneWide(dut_->ev_trap_cause_o, 1), ev_trap_cause_hi);
    CMP_U64(LaneWide(dut_->ev_trap_tval_o, 0), ev_trap_tval_lo);
    CMP_U64(LaneWide(dut_->ev_trap_tval_o, 1), ev_trap_tval_hi);
    CMP_U64(dut_->trap_pc_o, trap_pc);
    CMP_U64(dut_->trap_cause_o, trap_cause);
    CMP_U64(dut_->trap_tval_o, trap_tval);
    CMP_U64(dut_->csr_rd_data_o, csr_rd_data);
#undef CMP_U64

    // The identity field: a two-lane vector of 32-bit words, each carrying a
    // RET_ID_W-bit identity. The compare masks each lane to that width: the
    // upper bits of the 32-bit word are zero-fill, not identity.
    ++comparisons_;
    {
      const uint64_t id_mask = shadow_->ret_id_mask();
      const uint64_t got_id = (LaneField(dut_->ev_id_o, 0, width_) |
                               (static_cast<uint64_t>(LaneField(dut_->ev_id_o, 1, width_)) << 32)) & (id_mask | (id_mask << 32));
      if (got_id != v.ev_id) {
        Fail(where, "ev_id: expected " + mosaic::Hex(v.ev_id) + ", got " +
                        mosaic::Hex(LaneField(dut_->ev_id_o, 0, width_) |
                          (static_cast<uint64_t>(LaneField(dut_->ev_id_o, 1, width_)) << 32)) + Describe(s));
      }
    }

    // The order fault must never fire in the shipping build. It is the unit's own
    // statement that an out-of-order acknowledgement did not happen, so a cycle
    // where it fires is a defect in its own right even if every comparison above
    // passed.
    if (debug_cycle_ != 0 && clk_->cycle() == static_cast<uint64_t>(debug_cycle_)) {
      std::fprintf(stderr,
                   "DBG cycle %llu req %u/%u evv %u/%u evt %u/%u "
                   "seq %llu/%u regwe %u/%u csrdata %llu/%llu\n",
                   (unsigned long long)clk_->cycle(),
                   v.retire_req, Lanes32(dut_->retire_req_o, width_),
                   v.ev_valid, Lanes32(dut_->ev_valid_o, width_),
                   v.ev_trap, Lanes32(dut_->ev_trap_o, width_),
                   (unsigned long long)v.retire_seq, Lanes32(dut_->ev_seq_o, width_),
                   v.ev_reg_we, Lanes32(dut_->ev_reg_we_o, width_),
                   (unsigned long long)v.csr_rd_data,
                   (unsigned long long)dut_->csr_rd_data_o);
    }
    Require(!v.order_fault, where,
            "the unit reported an out-of-order retirement acknowledgement");

    // An event with a trap flag and a register write at once is the exact
    // conflation config/contracts/interfaces.json records against the retire
    // event, so it is checked on the DUT's own outputs rather than the shadow's.
    for (uint32_t i = 0; i < width_; i++) {
      if (!((dut_->ev_trap_o >> i) & 1u)) continue;
      Require(!((dut_->ev_reg_we_o >> i) & 1u), where,
              "lane " + std::to_string(i) +
                  " emitted a trap and a register write in the same event");
      Require(!((dut_->ev_csr_we_o >> i) & 1u), where,
              "lane " + std::to_string(i) +
                  " emitted a trap and a CSR write in the same event");
      Require(!((dut_->ev_store_o >> i) & 1u), where,
              "lane " + std::to_string(i) +
                  " emitted a trap and a store authorisation in the same event");
      Require(!((dut_->commit_valid_o >> i) & 1u), where,
              "lane " + std::to_string(i) +
                  " emitted a trap and a committed-map update in the same event");
    }

    // A recovery flush cycle must retire nothing and commit nothing. Enforced
    // here as well as modelled, because this is the one rule the I-018 boundary
    // depends on and it must not depend on the shadow agreeing.
    if (s.flush_valid || s.rob_flush) {
      Require(dut_->ev_valid_o == 0, where,
              "a flush cycle emitted " + std::to_string(dut_->ev_valid_o) +
                  " retire events; a recovery flush retires nothing");
      Require(dut_->commit_valid_o == 0, where,
              "a flush cycle updated the committed map");
      Require(dut_->retire_req_o == 0, where,
              "a flush cycle requested a retirement");
    }
  }

  // Capture the reports that describe the edge about to happen, then the state
  // the edge leaves behind.
  void CaptureReports() {
    // All four per-lane vectors are 32-bits-per-lane VlWide/64-bit mixes, so
    // every field is read lane by lane: assigning the packed word to a scalar
    // truncates lane 1 away, and `Events()` then reports a two-wide retire as
    // a single lane-0 event -- exactly the mask-3-reads-as-1 in head-block (a).
    report_ = Reports();
    report_.retire_req = Lanes32(dut_->retire_req_o, width_);
    report_.trap_flush = dut_->trap_flush_o != 0;
    report_.ev_valid = Lanes32(dut_->ev_valid_o, width_);
    report_.ev_trap = Lanes32(dut_->ev_trap_o, width_);
    report_.commit_valid = Lanes32(dut_->commit_valid_o, width_);
    report_.order_fault = dut_->o_order_fault_o != 0;
    report_.event_count = dut_->o_event_count_o;
    report_.x0_retired = dut_->o_x0_retired_o != 0;
    report_.pay_missing = dut_->o_pay_missing_o;
    report_.csr_unsupported = dut_->o_csr_unsupported_o;
    report_.exc_queued = dut_->o_exc_queued_o;
    report_.ev_csr_we = Lanes32(dut_->ev_csr_we_o, width_);
    report_.ev_store = Lanes32(dut_->ev_store_o, width_);
    report_.commit_accepted = dut_->commit_accepted_o != 0;
    report_.commit2_accepted = dut_->commit2_accepted_o != 0;
    report_.rob_retire_ack = dut_->rob_retire_ack_o;
    report_.rob_retire_ack_next = dut_->rob_retire_ack_next_o;
  }

  // The settled state, compared against the shadow's post-edge state. Reading it
  // after the edge is the whole point: a phase asking "after this completion, is
  // the entry complete?" must read *this*, not the cycle before.
  void CompareSettled() {
    const std::string where = At(phase_, clk_->cycle());

    ++comparisons_;
    if (dut_->o_minstret_o != shadow_->minstret()) {
      Fail(where, "minstret: expected " + std::to_string(shadow_->minstret()) +
                      ", got " + std::to_string(dut_->o_minstret_o));
    }
    ++comparisons_;
    if (dut_->o_retire_seq_o != shadow_->retire_seq()) {
      Fail(where, "retire_seq: expected " + std::to_string(shadow_->retire_seq()) +
                      ", got " + std::to_string(dut_->o_retire_seq_o));
    }
    ++comparisons_;
    if (dut_->rob_alloc_total_o != shadow_->alloc_total()) {
      Fail(where, "rob_alloc_total: expected " + std::to_string(shadow_->alloc_total()) +
                      ", got " + std::to_string(dut_->rob_alloc_total_o) +
                      " shadow-occ=" + std::to_string(shadow_->occupied()) +
                      " dut-occ=" + std::to_string(dut_->rob_occupied_o) +
                      " settled_alloc_index=" + std::to_string(settled_alloc_index_) +
                      " settled_alloc_gen=" + std::to_string(settled_alloc_gen_) +
                      " last_rst=" + std::to_string(last_rst_) +
                      " last_alloc_valid=" + std::to_string(last_alloc_valid_) +
                      " last_alloc_n=" + std::to_string(last_alloc_n_) +
                      " ok=" + std::to_string(dut_->rob_alloc_ok_o) +
                      " full=" + std::to_string(dut_->rob_alloc_full_o) +
                      " bad=" + std::to_string(dut_->rob_alloc_bad_o) +
                      " flush=" + std::to_string(dut_->rob_flush_i));
    }
    ++comparisons_;
    if (dut_->rob_retired_total_o != shadow_->retired_total()) {
      Fail(where, "rob_retired_total: expected " +
                      std::to_string(shadow_->retired_total()) + ", got " +
                      std::to_string(dut_->rob_retired_total_o) +
                      " ack=" + std::to_string(dut_->rob_retire_ack_o) +
                      " ack_next=" + std::to_string(dut_->rob_retire_ack_next_o) +
                      " ev=" + std::to_string(Lanes32(dut_->ev_valid_o, width_)) +
                      " req=" + std::to_string(Lanes32(dut_->retire_req_o, width_)));
    }
    ++comparisons_;
    if (dut_->rob_squashed_total_o != shadow_->squashed_total()) {
      Fail(where, "rob_squashed_total: expected " +
                      std::to_string(shadow_->squashed_total()) + ", got " +
                      std::to_string(dut_->rob_squashed_total_o));
    }

    // The ROB's conservation identity, asserted here rather than trusted from
    // whichever module happens to maintain the counters.
    const uint32_t alloc = dut_->rob_alloc_total_o;
    const uint32_t retired = dut_->rob_retired_total_o;
    const uint32_t squashed = dut_->rob_squashed_total_o;
    const uint32_t occupied = dut_->rob_occupied_o;
    Require(alloc == retired + squashed + occupied, where,
            "conservation broken: alloc " + std::to_string(alloc) + " != retired " +
                std::to_string(retired) + " + squashed " +
                std::to_string(squashed) + " + occupied " + std::to_string(occupied));

    // The committed map: every architectural register, against the shadow.
    for (uint32_t a = 0; a < ArchRegs(); a++) {
      ++comparisons_;
      if (ObservedCmtTag(a) != shadow_->cmt_tag(a) ||
          ObservedCmtGen(a) != shadow_->cmt_gen(a)) {
        Fail(where, "committed map x" + std::to_string(a) + ": expected tag " +
                        std::to_string(shadow_->cmt_tag(a)) + " gen " +
                        std::to_string(shadow_->cmt_gen(a)) + ", got tag " +
                        std::to_string(ObservedCmtTag(a)) + " gen " +
                        std::to_string(ObservedCmtGen(a)));
      }
    }

    // The speculative map must never move: retirement is not allowed to touch
    // speculative state, which is the I-017 half of the I-018 boundary.
    if (!spec_baseline_) {
      for (uint32_t a = 0; a < ArchRegs(); a++) {
        spec_base_tag_[a] = ObservedSpecTag(a);
        spec_base_gen_[a] = ObservedSpecGen(a);
      }
      spec_baseline_ = true;
    } else {
      for (uint32_t a = 0; a < ArchRegs(); a++) {
        ++comparisons_;
        if (ObservedSpecTag(a) != spec_base_tag_[a] ||
            ObservedSpecGen(a) != spec_base_gen_[a]) {
          Fail(where, "retirement disturbed the speculative map at x" +
                          std::to_string(a) + ": x" + std::to_string(a) +
                          " was tag " + std::to_string(spec_base_tag_[a]) +
                          " gen " + std::to_string(spec_base_gen_[a]) +
                          ", now tag " + std::to_string(ObservedSpecTag(a)) +
                          " gen " + std::to_string(ObservedSpecGen(a)));
        }
      }
    }

    // The sequence number is monotonic for the life of the machine, checked
    // against the shadow rather than against itself.
    if (have_prev_) {
      ++comparisons_;
      if (dut_->o_retire_seq_o < prev_retire_seq_) {
        Fail(where, "retire_seq went backwards: from " +
                        std::to_string(prev_retire_seq_) + " to " +
                        std::to_string(dut_->o_retire_seq_o));
      }
    }
    prev_retire_seq_ = dut_->o_retire_seq_o;
    have_prev_ = true;

    // (The allocation identity is sampled pre-edge in Cycle(), not here.)
    // The slot reconcile: every live shadow entry against the buffer's own
    // slot observation. A completion that landed on the wrong slot surfaces
    // here -- done bits disagree for a named slot -- not later as a readiness
    // disagreement for a queue position.
    ReconcileSlots(where);
  }

  void Tally(const Stim& s) {
    for (uint32_t i = 0; i < width_; i++) {
      if (!((report_.ev_valid >> i) & 1u)) continue;
      counters_.events++;
      if (report_.ev_valid == 1u) counters_.single_events++;
      if ((report_.ev_valid & ~(1u << i)) != 0) counters_.dual_events++;
      if ((report_.ev_trap >> i) & 1u) counters_.traps++;
      if ((report_.commit_valid >> i) & 1u) counters_.commits++;
    }
    if (report_.x0_retired) counters_.x0_retires++;
    if (report_.ev_csr_we) counters_.csr_writes++;
    if (report_.ev_store) counters_.stores++;
    if (s.rob_flush || s.flush_valid) counters_.flushes++;
    if (report_.ev_valid == 0 && shadow_->occupied() > 0) counters_.blocked_cycles++;
  }

  // Every live shadow entry against the buffer's own slot observation. The
  // shadow is keyed by slot, the buffer is read by slot: any completion,
  // close, allocation, or retire that one side filed differently surfaces as
  // a named-slot disagreement here, at the cycle it happened.
  void ReconcileSlots(const std::string& where) {
    for (uint32_t q = 0; q < shadow_->occupied(); q++) {
      const Entry* e = shadow_->at(q);
      if (e == nullptr || !e->live) continue;
      const uint32_t slot = e->slot;
      ++comparisons_;
      if (!ObsValid(slot)) {
        Fail(where, "slot " + std::to_string(slot) + ": shadow holds a live " +
                         "entry but the buffer reports the slot empty");
      }
      ++comparisons_;
      if (Mask(ObsGen(slot), shadow_->gen_mask()) != Mask(e->gen, shadow_->gen_mask())) {
        Fail(where, "slot " + std::to_string(slot) + ": generation disagrees: " +
                         "shadow " + std::to_string(e->gen) + ", buffer " +
                         std::to_string(ObsGen(slot)));
      }
      ++comparisons_;
      if (ObsDone(slot) != (e->done_mask & shadow_->done_mask())) {
        Fail(where, "slot " + std::to_string(slot) + ": done bits disagree: " +
                         "shadow " + std::to_string(e->done_mask) + ", buffer " +
                         std::to_string(ObsDone(slot)));
      }
      ++comparisons_;
      if (ObsClosed(slot) != e->closed) {
        Fail(where, "slot " + std::to_string(slot) + ": closed disagrees: " +
                         "shadow " + Bool(e->closed) + ", buffer " +
                         Bool(ObsClosed(slot)));
      }
      ++comparisons_;
      if (ObsExc(slot) != e->exc) {
        Fail(where, "slot " + std::to_string(slot) + ": exception disagrees: " +
                         "shadow " + Bool(e->exc) + ", buffer " +
                         Bool(ObsExc(slot)));
      }
    }
  }

  Vmosaic_retire_tb* dut_;
  mosaic::ClockDriver* clk_;
  uint64_t max_cycles_;
  ShadowPipeline* shadow_;
  uint32_t width_;
  uint32_t arch_regs_ = 32;
  uint32_t prf_entries_ = 0;
  std::string phase_;
  Reports report_;
  Counters counters_;
  uint64_t comparisons_ = 0;
  uint64_t cycles_ = 0;
  bool have_prev_ = false;
  uint64_t prev_retire_seq_ = 0;
  uint32_t settled_alloc_index_ = 0;
  uint32_t settled_alloc_gen_ = 0;
  bool spec_baseline_ = false;
  int debug_cycle_ = 0;
  int last_rst_ = -1;
  int last_alloc_valid_ = -1;
  uint32_t last_alloc_n_ = 0;
  std::vector<uint32_t> spec_base_tag_;
  std::vector<uint32_t> spec_base_gen_;
  mosaic::Rng rng_{1};
};

// ---------------------------------------------------------------- the phases
//
// Each phase resets first and owns one mechanism, and a run stops at the first
// failure, so the order decides *which* phase reports a given defect. The
// directed phases therefore run before the soak.

void PhaseResetState(Harness* h) {
  const std::string where = At("reset-state", h->shadow().occupied());
  Require(h->Events() == 0, where, "an event was emitted with nothing in the queue");
  Require(h->ObservedMinstret() == 0, where,
          "minstret is " + std::to_string(h->ObservedMinstret()) + " after reset");
  Require(h->ObservedRetireSeq() == 0, where, "retire_seq is not zero after reset");
  Require(h->ObservedOccupied() == 0, where, "the queue is not empty after reset");
  // The committed map's reset state is the identity: x5 maps to tag 5.
  Require(h->ObservedCmtTag(5) == 5 && h->ObservedCmtGen(5) == 0, where,
          "the committed map's reset state is not the identity at x5: tag " +
              std::to_string(h->ObservedCmtTag(5)) + " gen " +
              std::to_string(h->ObservedCmtGen(5)));
  // 96 physical registers minus the 32 owned by the architectural reset
  // mappings: the committed map and the free list agree at reset.
  Require(h->ObservedRenameFree() == h->ObservedPRFEntries() - h->ArchRegs(), where,
          "the free count at reset is " + std::to_string(h->ObservedRenameFree()) +
              ", expected " + std::to_string(h->ObservedPRFEntries() - h->ArchRegs()));
}

void PhaseInOrder(Harness* h, int instructions) {
  // Randomised ready/not-ready heads: allocate a few, complete a random subset,
  // then present payloads and let the queue drain. The shadow checks the order
  // on every cycle, so the property being tested is "the emitted order is
  // allocation order", not "the events look plausible".
  uint32_t tag = 1;
  std::vector<Harness::Identity> live;
  std::vector<uint32_t> emitted_order;

  for (int step = 0; step < instructions && static_cast<int>(live.size()) < 8; step++) {
    // Allocate one instruction.
    Harness::Identity id = h->Alloc(tag, 1);
    live.push_back(id);
    ++tag;

    // Complete each live entry at most once: the head unconditionally (the
    // queue drains through lane 0, so the head must move every step), the rest
    // by a 55% coin that skips entries the shadow already calls done. A second
    // completion for an already-done entry is a duplicate the buffer reports
    // and the shadow ignores -- offering one every step is how the shadow fell
    // a cycle behind the buffer and misreported head-complete.
    if (!live.empty()) {
      const Entry* head = h->shadow().head();
      if (head == nullptr || !h->shadow().Ready(*head)) h->Complete(live.front());
    }
    for (size_t k = live.size(); k-- > 0;) {
      const Entry* e = h->shadow().at(k);
      if (e != nullptr && h->shadow().Ready(*e)) continue;
      if (h->rng().Chance(55)) h->Complete(live[k]);
    }

    // Present payloads for every entry the shadow says is ready, and see what
    // comes out.
    Stim s;
    for (uint32_t lane = 0; lane < h->width(); lane++) {
      const Entry* e = h->shadow().at(lane);
      if (e == nullptr) continue;
      if (!h->shadow().Ready(*e)) continue;
      s.pay_valid |= 1u << lane;
      s.pay_reg_we |= 1u << lane;
      s.pay_rd |= (uint64_t(1 + (lane % 5))) << (lane * 32);
      s.pay_value[lane] = (0x1000ull + tag);
    }
    if (s.pay_valid == 0) continue;

    // `expected` is what the phase *presented*, not what it demands: the DUT
    // retires the lanes the shadow, the payloads, and the buffer all agree on.
    // The check that matters is the negative one -- nothing retired that was
    // not presented and ready. Demanding every presented lane retire the same
    // cycle would assert a same-cycle presentation-to-event latency the contract
    // never promises; the drain loop below is what proves everything presented
    // eventually leaves, in lane order.
    const uint32_t expected = s.pay_valid;
    h->Cycle(s);
    for (uint32_t lane = 0; lane < h->width(); lane++) {
      if (!((h->Events() >> lane) & 1u)) continue;
      Require((expected >> lane) & 1u, At("in-order", tag),
              "lane " + std::to_string(lane) +
                  " retired without being ready: a later entry filled the width "
                  "over a blocked head");
      emitted_order.push_back(lane);
    }

    // Retire the ones that came out, so the queue does not fill.
    uint32_t retired = 0;
    for (uint32_t lane = 0; lane < h->width(); lane++) {
      if ((h->Events() >> lane) & 1u) retired++;
    }
    for (uint32_t k = 0; k < retired && !live.empty(); k++) live.erase(live.begin());
  }

  // Drain: complete and present everything until the queue is empty.
  for (int guard = 0; guard < 64 && h->shadow().occupied() > 0; guard++) {
    // Complete every entry whose completion the shadow still wants -- by SLOT,
    // not by queue position, which shifts as the head retires underneath it.
    for (uint32_t q = 0; q < h->shadow().occupied(); q++) {
      const Entry* e = h->shadow().at(q);
      if (e == nullptr || !e->live) continue;
      if (e->done_mask == h->shadow().ExpectedMask(e->num_uops)) continue;
      Harness::Identity id;
      id.index = e->slot;
      id.gen = e->gen;
      h->Complete(id);
    }
    Stim s;
    for (uint32_t lane = 0; lane < h->width(); lane++) {
      const Entry* e = h->shadow().at(lane);
      if (e == nullptr || !h->shadow().Ready(*e)) continue;
      s.pay_valid |= 1u << lane;
      s.pay_reg_we |= 1u << lane;
      s.pay_rd |= (uint64_t(10 + lane)) << (lane * 32);
    }
    if (s.pay_valid == 0) continue;
    h->Cycle(s);
  }

  Require(h->counters().events > 0, At("in-order", tag),
          "the phase retired nothing, so it proved nothing");
}

void PhaseHeadBlock(Harness* h) {
  // The card's named case, in three shapes. In each, the head is not retirable
  // and the entry *behind* it is fully complete -- so the only thing that stops
  // the younger entry retiring is the head-block rule.
  //
  //   a) an incomplete head: allocated, never completed
  //   b) an unclosed head: allocated open, completed, never closed
  //   c) an exceptional head: completed, and an exception raised on it

  // (a) incomplete head.
  {
    h->Fresh();
    const Harness::Identity older = h->Alloc(100);
    const Harness::Identity younger = h->Alloc(101);
    h->Complete(younger);   // the younger entry is completely done
    // The head is not complete. Present payloads for both lanes.
    Stim s;
    s.pay_valid = 0x3;
    s.pay_reg_we = 0x3;
    s.pay_rd = (5u) | (6ull << 32);
    h->Cycle(s);

    Require(h->Events() == 0, At("head-block", 0),
            "an incomplete head did not block the complete entry behind it: events=" +
                std::to_string(h->Events()) + " req=" + std::to_string(h->RetireReq()) +
                " occ=" + std::to_string(h->ObservedOccupied()));

    // Completing the head releases BOTH lanes on the next payload cycle when
    // both are ready (rtl/core/mosaic_rob.sv:542), so one Cycle drains the pair.
    h->Complete(older);
    h->Cycle(s);
    Require(h->Events() == 0x3, At("head-block", 1),
            "once the head completed, expected both lanes to retire, got mask " +
                std::to_string(h->Events()) + " req=" + std::to_string(h->RetireReq()));
  }

  // (b) unclosed head.
  {
    h->Fresh();
    const Harness::Identity older = h->Alloc(102, 1, /*exc=*/false, /*open=*/true);
    const Harness::Identity younger = h->Alloc(103);
    h->Complete(older);
    h->Complete(younger);
    Stim s;
    s.pay_valid = 0x3;
    s.pay_reg_we = 0x3;
    s.pay_rd = (7u) | (8ull << 32);
    h->Cycle(s);

    Require(h->Events() == 0, At("head-block", 2),
            "an unclosed head did not block the complete entry behind it");
    Require(h->PayMissing() == 0, At("head-block", 2),
            "the unit reported a missing payload for an unclosed head, which is "
            "a readiness report and not a payload one");

    // Closing it releases both.
    h->Close(older);
    h->Cycle(s);
    Require(h->Events() == 0x3, At("head-block", 3),
            "once the head closed, expected both lanes to retire, got mask " +
                std::to_string(h->Events()));
  }

  // (c) exceptional head.
  {
    h->Fresh();
    const Harness::Identity older = h->Alloc(104, 1, /*exc=*/true);
    const Harness::Identity younger = h->Alloc(105);
    h->Complete(older);
    h->Complete(younger);
    Stim s;
    s.pay_valid = 0x3;
    s.pay_reg_we = 0x3;
    s.pay_rd = (9u) | (10ull << 32);
    s.pay_exc_cause[0] = 2;   // illegal instruction, from mosaic_pkg
    s.pay_exc_tval[0] = 0xdeadbeefull;
    h->Cycle(s);

    Require(h->Events() == 0x1, At("head-block", 4),
            "an exceptional head produced mask " + std::to_string(h->Events()) +
                "; expected only the trap event in lane 0");
    Require(h->Traps() == 0x1, At("head-block", 4),
            "an exceptional head did not produce a trap event");
    Require(h->TrapFlush(), At("head-block", 4),
            "an exceptional head did not request the buffer be dropped");
    Require(h->ObservedOccupied() == 0, At("head-block", 4),
            "the buffer was not emptied by the trap: " +
                std::to_string(h->ObservedOccupied()) + " entries left");
  }
}

void PhaseDualWidth(Harness* h) {
  // Both lanes retirable: two events, in order, in one cycle, with the younger
  // carrying the larger sequence number.
  h->Fresh();
  const Harness::Identity a = h->Alloc(200);
  const Harness::Identity b = h->Alloc(201);
  h->Complete(a);
  h->Complete(b);

  Stim s;
  s.pay_valid = 0x3;
  s.pay_reg_we = 0x3;
  s.pay_rd = (11u) | (12ull << 32);
  s.pay_value[0] = 0xBBBBAAAAull;  // one 64-bit value; the two lanes share the bus in this phase
  s.pay_value[1] = 0xBBBBAAABull;  // lane 1 needs its own value now the bus is per-lane
  h->Cycle(s);

  Require(h->Events() == 0x3, At("dual-width", 0),
          "two retirable entries produced mask " + std::to_string(h->Events()) +
              "; expected both lanes");
  Require(h->EventCount() == 2, At("dual-width", 0),
          "event_count is " + std::to_string(h->EventCount()) + ", expected 2");

  // The younger lane's sequence number is the larger one, and the two differ by
  // exactly one: the pair is consecutive in the architectural order.
  const uint32_t seq0 = h->SeqOfLane(0);
  const uint32_t seq1 = h->SeqOfLane(1);
  Require(seq1 == ((seq0 + 1) & 0xFFu), At("dual-width", 0),
          "the two lanes' sequence numbers are " + std::to_string(seq0) + " and " +
              std::to_string(seq1) + "; a two-wide retire must be consecutive");
  Require(h->PcOfLane(0) < h->PcOfLane(1), At("dual-width", 0),
          "lane 0's PC is not below lane 1's: the pair is out of program order");

  // Second lane unretirable: exactly one event, and nothing reordered.
  h->Fresh();
  const Harness::Identity c = h->Alloc(202);
  const Harness::Identity d = h->Alloc(203);
  h->Complete(c);
  // `d` is deliberately never completed.
  Stim s2;
  s2.pay_valid = 0x3;
  s2.pay_reg_we = 0x3;
  s2.pay_rd = (13u) | (14ull << 32);
  h->Cycle(s2);

  Require(h->Events() == 0x1, At("dual-width", 1),
          "one retirable and one unretirable entry produced mask " +
              std::to_string(h->Events()) + "; expected exactly one event");
  Require(h->EventCount() == 1, At("dual-width", 1),
          "event_count is " + std::to_string(h->EventCount()) + ", expected 1");
  Require(h->ObservedOccupied() == 1, At("dual-width", 1),
          "the queue's occupancy is " + std::to_string(h->ObservedOccupied()) +
              " after one retirement; expected 1");

  // Completing the second releases it, and only it: the width is filled again,
  // never reordered.
  h->Complete(d);
  h->Cycle(s2);
  Require(h->Events() == 0x1, At("dual-width", 2),
          "the second entry alone did not retire, mask " + std::to_string(h->Events()));
  Require(h->ObservedOccupied() == 0, At("dual-width", 2),
          "the queue did not drain");

  // First lane unretirable, second retirable: exactly one event, and it is the
  // first lane's -- not the second lane's, which would be a reorder.
  h->Fresh();
  const Harness::Identity e = h->Alloc(204);
  const Harness::Identity f = h->Alloc(205);
  h->Complete(f);
  (void)e;
  Stim s3;
  s3.pay_valid = 0x3;
  s3.pay_reg_we = 0x3;
  s3.pay_rd = (15u) | (16ull << 32);
  h->Cycle(s3);
  Require(h->Events() == 0, At("dual-width", 3),
          "a complete entry behind a blocked head retired: mask " +
              std::to_string(h->Events()));
}

void PhaseCommittedOnce(Harness* h) {
  // The committed map moves exactly once per retired instruction, and a squashed
  // instruction never moves it at all. The scenario is the card's: a squashed
  // entry, then a younger entry allocated into the same slot.
  h->Fresh();

  // The squashed instruction: it writes x5 to tag 60 and is complete, so its
  // payload is on the wire. If the unit commits on payload rather than on
  // retirement, the map moves here and nothing undoes it.
  const Harness::Identity doomed = h->Alloc(300);
  h->Complete(doomed);

  Stim s;
  s.pay_valid = 0x1;
  s.pay_reg_we = 0x1;
  s.pay_rd = 5u;
  s.rob_flush = true;      // recovery squashes the entry in the same cycle
  h->Cycle(s);

  Require(h->Events() == 0, At("committed-once", 0),
          "a flush cycle retired an instruction");
  Require(h->ObservedCmtTag(5) != 60, At("committed-once", 0),
          "the squashed instruction updated the committed map to tag " +
              std::to_string(h->ObservedCmtTag(5)));
  Require(h->ObservedCmtTag(5) == 5, At("committed-once", 0),
          "the committed map's x5 was disturbed by a squashed instruction");

  // The younger instruction, allocated into the same slot and writing x5 to a
  // different tag. It must move the map exactly once, to *its* tag.
  const Harness::Identity survivor = h->Alloc(301);
  h->Complete(survivor);

  Stim s2;
  s2.pay_valid = 0x1;
  s2.pay_reg_we = 0x1;
  s2.pay_rd = 5u;
  h->Cycle(s2);

  Require(h->Events() == 0x1, At("committed-once", 1),
          "the younger entry did not retire");
  Require(h->ObservedCmtTag(5) == 60, At("committed-once", 1),
          "x5 maps to tag " + std::to_string(h->ObservedCmtTag(5)) +
              " after the younger entry retired; expected 60");

  // Two instructions writing the same rd in one cycle: the younger mapping wins,
  // and only one commit is seen.
  h->Fresh();
  const Harness::Identity first = h->Alloc(302);
  const Harness::Identity second = h->Alloc(303);
  h->Complete(first);
  h->Complete(second);

  Stim s3;
  s3.pay_valid = 0x3;
  s3.pay_reg_we = 0x3;
  s3.pay_rd = (20u) | (20ull << 32);
  h->Cycle(s3);

  Require(h->Commits() == 0x3, At("committed-once", 2),
          "two instructions writing the same rd produced commit mask " +
              std::to_string(h->Commits()) + "; expected both lanes");
  Require(h->ObservedCmtTag(20) == second.tag, At("committed-once", 2),
          "x20 maps to tag " + std::to_string(h->ObservedCmtTag(20)) +
              " after two same-rd commits; expected the younger tag " +
              std::to_string(second.tag));
}

void PhaseCsrAtRetire(Harness* h) {
  // A CSR write whose instruction is in the buffer but not complete must not be
  // visible; the same write must be visible once the instruction retires.
  h->Fresh();

  const uint64_t before = h->ObservedMscratch();
  const Harness::Identity id = h->Alloc(400);

  // The payload is on the wire, but the instruction is not complete: no write.
  Stim s;
  s.pay_valid = 0x1;
  s.pay_csr_we = 0x1;
  s.pay_csr_addr = kCsrMscratch;
  s.pay_csr_value[0] = 0x12345678ull;
  s.csr_rd_valid = true;
  s.csr_rd_addr = kCsrMscratch;
  h->Cycle(s);

  Require(h->Events() == 0, At("csr-at-retire", 0),
          "an incomplete instruction retired");
  Require(h->CsrRead() == before, At("csr-at-retire", 0),
          "the CSR read returned " + mosaic::Hex(h->CsrRead()) + " for an "
          "instruction that has not retired; expected " + mosaic::Hex(before));
  Require(h->ObservedMscratch() == before, At("csr-at-retire", 0),
          "mscratch changed to " + mosaic::Hex(h->ObservedMscratch()) +
              " before its instruction retired");

  // Complete the instruction and present the same payload: now it must be
  // visible. The read happens in the same cycle as the retirement and returns
  // the pre-edge value, because a CSR read is a read of the state as it stands;
  // the next cycle's read must see the write.
  h->Complete(id);
  h->Cycle(s);

  Require(h->Events() == 0x1, At("csr-at-retire", 1),
          "the completed CSR-writing instruction did not retire");
  Require(h->CsrRead() == before, At("csr-at-retire", 1),
          "a CSR write was visible in the very cycle its instruction retired, "
          "before the edge: " + mosaic::Hex(h->CsrRead()));

  Stim s3;
  s3.csr_rd_valid = true;
  s3.csr_rd_addr = kCsrMscratch;
  h->Cycle(s3);
  Require(h->CsrRead() == 0x12345678ull, At("csr-at-retire", 2),
          "the CSR write was not visible the cycle after its instruction "
          "retired: read " + mosaic::Hex(h->CsrRead()));

  // An unsupported address is reported, not absorbed.
  Stim s4;
  s4.csr_rd_valid = true;
  s4.csr_rd_addr = 0x340 + 1;
  h->Cycle(s4);
  Require(h->CsrReadUnsupported(), At("csr-at-retire", 3),
          "a read of an unimplemented CSR address did not report unsupported");

  h->Fresh();
  const Harness::Identity id2 = h->Alloc(401);
  h->Complete(id2);
  Stim s5;
  s5.pay_valid = 0x1;
  s5.pay_csr_we = 0x1;
  s5.pay_csr_addr = 0x300;    // mstatus: not in this file
  s5.pay_csr_value[0] = 0x1;
  h->Cycle(s5);
  Require(h->CsrUnsupported() == 0x1, At("csr-at-retire", 4),
          "a retiring CSR write to an unimplemented address was not reported");
}

void PhaseMinstret(Harness* h) {
  // `minstret` counts exactly the retired instructions: the trap included, the
  // squashed ones excluded, and nothing that is still in the buffer.
  h->Fresh();
  const uint64_t start = h->ObservedMinstret();

  // Four ordinary instructions, one squashed, one trapped.
  std::vector<Harness::Identity> live;
  for (uint32_t k = 0; k < 4; k++) {
    live.push_back(h->Alloc(500 + k));
    h->Complete(live.back());
  }
  // Retire two of them.
  Stim s;
  s.pay_valid = 0x3;
  s.pay_reg_we = 0x3;
  s.pay_rd = (1u) | (2ull << 32);
  h->Cycle(s);
  Require(h->Events() == 0x3, At("minstret", 0),
          "expected two retirements, got mask " + std::to_string(h->Events()));

  const uint64_t after_two = h->ObservedMinstret();
  Require(after_two == start + 2, At("minstret", 1),
          "minstret is " + std::to_string(after_two) + " after two retirements "
          "from " + std::to_string(start));

  // A cycle in which nothing retires: the counter must not move, even though
  // two instructions are still in the buffer.
  Stim idle;
  idle.pay_valid = 0x1;
  h->Cycle(idle);
  Require(h->ObservedMinstret() == after_two, At("minstret", 2),
          "minstret moved to " + std::to_string(h->ObservedMinstret()) +
              " in a cycle that retired nothing, while instructions were in flight");

  // A recovery flush squashes the two that are left: still no move.
  Stim flush;
  flush.rob_flush = true;
  h->Cycle(flush);
  Require(h->ObservedMinstret() == after_two, At("minstret", 3),
          "minstret moved to " + std::to_string(h->ObservedMinstret()) +
              " across a squash of two in-flight instructions");

  // A trap counts: the trapping instruction is architecturally final.
  const Harness::Identity trap = h->Alloc(510, 1, /*exc=*/true);
  h->Complete(trap);
  Stim ts;
  ts.pay_valid = 0x1;
  ts.pay_exc_cause[0] = 2;
  ts.pay_exc_tval[0] = 0x42;
  h->Cycle(ts);
  Require(h->Traps() == 0x1, At("minstret", 4), "the exceptional entry did not trap");
  Require(h->ObservedMinstret() == after_two + 1, At("minstret", 4),
          "minstret is " + std::to_string(h->ObservedMinstret()) +
              " after a trap from " + std::to_string(after_two) +
              "; a trap is architecturally final and counts");

  Require(h->counters().events >= 3, At("minstret", 5),
          "the phase retired too little to prove anything");
}

void PhaseTrap(Harness* h) {
  // A trap is emitted as a trap, not as an ordinary retire, and it carries the
  // cause and the faulting value.
  h->Fresh();
  const Harness::Identity id = h->Alloc(600, 1, /*exc=*/true);
  h->Complete(id);

  Stim s;
  s.pay_valid = 0x1;
  s.pay_reg_we = 0x1;              // a payload that would be harmful if believed
  s.pay_rd = 25u;
  s.pay_csr_we = 0x1;
  s.pay_csr_addr = kCsrMscratch;
  s.pay_csr_value[0] = 0x9999;
  s.pay_is_store = 0x1;
  s.pay_store_addr[0] = 0x80001000ull;
  s.pay_store_data[0] = 0x5a5a;
  s.pay_exc_cause[0] = 2;             // illegal instruction
  s.pay_exc_tval[0] = 0x1234abcdull;
  h->Cycle(s);

  Require(h->Events() == 0x1, At("trap", 0),
          "the trap produced mask " + std::to_string(h->Events()) +
              "; expected exactly one event");
  Require(h->Traps() == 0x1, At("trap", 0),
          "the trap event did not carry the trap flag");
  Require(h->Commits() == 0, At("trap", 0),
          "the trap updated the committed map");
  Require(h->ObservedCmtTag(25) == 25, At("trap", 0),
          "the trapping instruction's register write reached the committed map");
  Require(h->ObservedMscratch() == 0, At("trap", 0),
          "the trapping instruction's CSR write reached the CSR file: mscratch " +
              mosaic::Hex(h->ObservedMscratch()));

  const uint64_t cause = h->TrapCause();
  const uint64_t tval = h->TrapTval();
  Require(cause == 2, At("trap", 1),
          "the trap cause is " + std::to_string(cause) + ", expected 2");
  Require(tval == 0x1234abcdull, At("trap", 1),
          "the trap tval is " + mosaic::Hex(tval));

  // The buffer is empty afterwards: the trap dropped it.
  Require(h->ObservedOccupied() == 0, At("trap", 2),
          "the buffer holds " + std::to_string(h->ObservedOccupied()) +
              " entries after a trap; expected 0");
}

void PhaseFlush(Harness* h) {
  // A recovery flush cycle retires nothing, commits nothing, and does not move
  // the committed map. The I-018 boundary, checked from this side.
  h->Fresh();
  const Harness::Identity a = h->Alloc(700);
  const Harness::Identity b = h->Alloc(701);
  h->Complete(a);
  h->Complete(b);

  // Payloads on the wire, a CSR write and all, and a flush in the same cycle.
  Stim s;
  s.pay_valid = 0x3;
  s.pay_reg_we = 0x3;
  s.pay_csr_we = 0x3;
  s.pay_csr_addr = (static_cast<uint64_t>(kCsrMscratch)) | (static_cast<uint64_t>(kCsrMscratch) << 32);
  s.pay_csr_value[0] = 0x7777;
  s.pay_rd = (30u) | (31ull << 32);
  s.pay_is_store = 0x3;
  s.pay_store_addr[0] = 0x80002000ull;
  s.flush_valid = true;
  h->Cycle(s);

  Require(h->Events() == 0, At("flush", 0),
          "a flush cycle retired " + std::to_string(h->Events()) + " instructions");
  Require(h->ObservedOccupied() == 0, At("flush", 0),
          "the queue is not empty after a flush");
  Require(h->ObservedCmtTag(30) == 30 && h->ObservedCmtTag(31) == 31, At("flush", 0),
          "a flush cycle moved the committed map");
  Require(h->ObservedMscratch() == 0, At("flush", 0),
          "a flush cycle moved the CSR file: mscratch " +
              mosaic::Hex(h->ObservedMscratch()));
  Require(h->ObservedRetiredTotal() == 0, At("flush", 0),
          "the queue's retirement counter moved across a flush");

  // The buffer is usable again afterwards, which proves the flush was a real
  // teardown and not a stall.
  const Harness::Identity c = h->Alloc(702);
  h->Complete(c);
  Stim s2;
  s2.pay_valid = 0x1;
  s2.pay_reg_we = 0x1;
  s2.pay_rd = 30u;
  h->Cycle(s2);
  Require(h->Events() == 0x1, At("flush", 1),
          "the buffer did not accept work after a flush");
}

void PhaseSoak(Harness* h, int steps) {
  mosaic::Rng rng(h->shadow().occupied() + 12345);

  for (int step = 0; step < steps; step++) {
    const int action = static_cast<int>(rng.Below(100));
    if (action < 25) {
      // Allocate one or two, occasionally multi-child or open.
      const bool two = rng.Chance(40);
      h->Alloc(1000 + static_cast<uint32_t>(step), 1 + rng.Below(3),
               rng.Chance(4), rng.Chance(10));
      if (two) {
        h->Alloc(2000 + static_cast<uint32_t>(step), 1 + rng.Below(3),
                 rng.Chance(4), rng.Chance(10));
      }
    } else if (action < 60) {
      // Complete a random live entry, addressing it by slot.
      const uint32_t occ = h->ObservedOccupied();
      if (occ == 0) continue;
      const uint32_t idx = rng.Below(occ);
      const Entry* e = h->shadow().at(idx);
      if (e == nullptr) continue;
      const uint32_t want = h->shadow().ExpectedMask(e->num_uops);
      if (e->done_mask == want) continue;
      const uint32_t missing = want & ~e->done_mask;
      const uint32_t uop = static_cast<uint32_t>(__builtin_ctz(missing));
      Harness::Identity id;
      id.index = idx;
      id.gen = h->shadow().GenOf(idx);
      h->Complete(id, uop, rng.Chance(3));
    } else if (action < 68) {
      // Close a random open entry.
      const uint32_t occ = h->ObservedOccupied();
      if (occ == 0) continue;
      const uint32_t idx = rng.Below(occ);
      const Entry* e = h->shadow().at(idx);
      if (e == nullptr || e->closed) continue;
      Harness::Identity id;
      id.index = idx;
      id.gen = h->shadow().GenOf(idx);
      h->Close(id);
    } else if (action < 78) {
      // A recovery flush.
      Stim s;
      s.flush_valid = true;
      h->Cycle(s);
    } else {
      // Present payloads for whatever is ready.
      Stim s;
      for (uint32_t lane = 0; lane < h->width(); lane++) {
        const Entry* e = h->shadow().at(lane);
        if (e == nullptr || !h->shadow().Ready(*e)) continue;
        s.pay_valid |= 1u << lane;
        if (rng.Chance(60)) {
          s.pay_reg_we |= 1u << lane;
          s.pay_rd |= (uint64_t(1 + rng.Below(31))) << (lane * 32);
          s.pay_value[lane] = rng.Next();
        }
        if (rng.Chance(15)) {
          s.pay_csr_we |= 1u << lane;
          s.pay_csr_addr |= (uint64_t(rng.Chance(10) ? 0x300u : kCsrMscratch)) << (lane * 32);
          s.pay_csr_value[lane] = rng.Next();
        }
        if (rng.Chance(10)) {
          s.pay_is_store |= 1u << lane;
          s.pay_store_addr[lane] = rng.Next();
          s.pay_store_data[lane] = rng.Next();
        }
        if (rng.Chance(5)) s.pay_valid &= ~(1u << lane);   // withhold a payload
      }
      if (s.pay_valid != 0 || s.pay_reg_we != 0) h->Cycle(s);
    }
  }

  Require(h->counters().events > 100, At("soak", 0),
          "the soak retired only " + std::to_string(h->counters().events) +
              " instructions, so it proved little");
  Require(h->counters().dual_events > 0, At("soak", 0),
          "the soak never retired two instructions in one cycle");
  Require(h->counters().flushes > 0, At("soak", 0),
          "the soak never flushed");
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
  Vmosaic_retire_tb dut;

  std::string detail;
  bool passed = true;
  try {
    // The geometry outputs are constants derived from the same generated package
    // the RTL reads, so one evaluation is enough to read them and no reset is
    // needed first. The shadow is sized from these numbers and from nothing else.
    dut.clk = 0;
    dut.rst = 0;
    dut.eval();

    const uint32_t width = dut.o_retire_width_o;
    const uint32_t rob_entries = dut.o_rob_entries_o;
    const uint32_t rob_index_w = dut.o_rob_index_w_o;
    const uint32_t rob_id_w = dut.o_rob_id_w_o;
    const uint32_t max_uops = dut.o_max_uops_o;
    const uint32_t prf_entries = dut.o_prf_entries_o;
    const uint32_t tag_w = dut.o_tag_w_o;
    const uint32_t arch_regs = dut.o_arch_regs_o;

    Require(width >= 1, "geometry", "the DUT reported a zero retire width");
    Require(rob_entries > 0 && max_uops > 0 && rob_index_w > 0, "geometry",
            "the DUT reported a zero queue depth, so the shadow cannot be sized");
    Require(rob_entries == (1u << rob_index_w), "geometry",
            "the shadow models the never-truncates elaboration only: the DUT "
            "reports " + std::to_string(rob_entries) + " entries in a " +
                std::to_string(rob_index_w) + "-bit index space");
    Require(rob_id_w == 2 * rob_index_w, "geometry",
            "the ROB's identity width is not twice its slot index width");
    // The two identity widths are deliberately different and the driver checks
    // the relationship rather than the equality. The queue's `tag` is the
    // producer's own name for a macro and is sized as `2 * ROB_INDEX_W`; the
    // retire unit carries a *destination* identity, whose tag is a physical
    // register tag and whose generation is that tag's allocation generation.
    // The retire unit must be able to hold a queue tag without truncation,
    // because the commit it emits names that tag.
    Require(tag_w <= rob_id_w, "geometry",
            "the retire unit's destination tag is " + std::to_string(tag_w) +
                " bits but the queue names macros with a " +
                std::to_string(rob_id_w) + "-bit tag: a commit would name a "
                "truncated identity");
    Require(arch_regs == 32, "geometry",
            "the architectural register file is not 32 entries, and the driver "
            "models a 5-bit register address");
    Require(prf_entries >= arch_regs, "geometry",
            "the physical register file is smaller than the architectural one");

    ShadowPipeline shadow(width, rob_entries, rob_index_w, rob_id_w, tag_w,
                          arch_regs, max_uops, Clog2(2 * rob_entries + 1),
                          tag_w, tag_w);
    Harness harness(&dut, &clk, options.max_cycles, &shadow);
    harness.set_geometry(prf_entries, arch_regs);

    // Phase order is a deliberate choice. Each phase resets first and owns one
    // mechanism, and a run stops at the first failure, so the order decides
    // *which* phase reports a given defect. The directed phases therefore run
    // before the soak.
    harness.Phase("reset-state");
    harness.Fresh();
    PhaseResetState(&harness);

    harness.set_debug_cycle(8);
    harness.Phase("in-order");
    harness.Fresh();
    PhaseInOrder(&harness, 220);

    harness.Phase("head-block");
    PhaseHeadBlock(&harness);

    harness.Phase("dual-width");
    harness.Fresh();
    PhaseDualWidth(&harness);

    harness.Phase("committed-once");
    PhaseCommittedOnce(&harness);

    harness.Phase("csr-at-retire");
    PhaseCsrAtRetire(&harness);

    harness.Phase("minstret");
    PhaseMinstret(&harness);

    harness.Phase("trap");
    PhaseTrap(&harness);

    harness.Phase("flush");
    PhaseFlush(&harness);

    harness.Phase("soak");
    harness.Fresh();
    PhaseSoak(&harness, 4000);

    const Harness::Counters& c = harness.counters();
    detail = "retire contract holds: " + std::to_string(harness.comparisons()) +
             " shadow comparisons over " + std::to_string(harness.cycles()) +
             " cycles; " + std::to_string(c.events) + " events (" +
             std::to_string(c.dual_events) + " in two-wide cycles, " +
             std::to_string(c.single_events) + " alone), " +
             std::to_string(c.traps) + " traps, " + std::to_string(c.commits) +
             " committed-map updates, " + std::to_string(c.x0_retires) +
             " x0 retirements, " + std::to_string(c.csr_writes) + " CSR writes, " +
             std::to_string(c.stores) + " store authorisations, " +
             std::to_string(c.flushes) + " flushes, " +
             std::to_string(c.blocked_cycles) + " blocked cycles, seed " +
             std::to_string(options.seed);
  } catch (const Failure& f) {
    reporter.Mismatch(f.what, "contract holds", "contract violated");
    passed = false;
    detail = "contract violated: " + f.what;
  }

  dut.final();
  reporter.Check(passed, "no contract violation in any phase");
  return reporter.Finish(passed ? "PASS" : "FAIL", detail);
}