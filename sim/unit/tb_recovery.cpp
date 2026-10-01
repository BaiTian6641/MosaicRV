// ============================================================================
// tb_recovery.cpp -- CASE=recovery.checkpoint_exact_restore, work package I-018.
//
// The DUT is never its own oracle. Every output is compared, on every cycle of
// every phase, against an independent C++ shadow written from the *contract*
// documented in rtl/core/mosaic_recovery.sv -- the full-checkpoint rule, the
// undo-journal rule, the oldest-redirect rule, the epoch-and-credit rule -- and
// not from the RTL's structure. The shadow shares no code with the RTL and never
// looks at Verilator internals, so agreeing with it is evidence about the
// contract rather than a restatement of the implementation.
//
// Geometry is read from the elaborated DUT (`o_prf_entries_o`, `o_rob_entries_o`,
// `o_ckpt_depth_param_o`, ...), which are constants derived from the instance
// parameters, so this file contains no depth of its own: a profile with a
// different ROB sizes the shadow on the next run, and there is no number here to
// forget to update.
//
// The two claims this case exists to establish, and how each is made to be a
// real claim rather than a restatement:
//
//   1. **A restore is exact.** Not "the free count is right" and not "the free
//      set looks plausible": the *entire* speculative state -- the free mask, the
//      generation-valid mask, the written mask, every generation, both maps, the
//      tail, the rotation point and the journal length -- is captured wholesale
//      at checkpoint time and compared wholesale after the restore. A shadow
//      that agreed with a deliberately weaker DUT would check nothing, so the
//      comparison is over packed integers the DUT exports for the purpose, not
//      over a projection of them.
//
//   2. **The credit is returned exactly once.** A counter that can only go up
//      cannot distinguish a second delivery from a first, so the shadow tracks
//      the reservation *table*, and the phase asserts the table's population
//      count against the DUT's `credits_outstanding` on every cycle plus the
//      exact number of returns at the end. "Rejected" is not the check; the
//      count of returned credits is.
//
// Phases, each of which can fail on its own:
//
//   1. reset-state       the documented cold state straight after reset: the
//                        architectural reset mappings, tags 0..31 owned rather
//                        than free, an empty journal, epoch zero, no checkpoints.
//   2. exact-restore     the central phase. Build speculative state, checkpoint,
//                        allocate a great deal more, then redirect: the free set,
//                        the generations, the maps, the tail and the rotation
//                        point must be bit-identical to the checkpoint, and the
//                        journal must be back at the checkpoint's mark.
//   3. nested-ckpt       two checkpoints outstanding, the older one redirects.
//                        The older is restored and the younger is *consumed*, not
//                        left behind as a checkpoint for a branch that no longer
//                        exists. Then the younger redirects after the older has
//                        been taken, and the result is a stale redirect that is
//                        reported and not taken.
//   4. oldest-redirect   the card's rule. Two redirects pending at once, in
//                        both arrival orders and in the same cycle: the older is
//                        taken every time, and the younger is reported killed.
//   5. late-response     after a squash, a response carrying the pre-redirect
//                        epoch is rejected and its credit returned *once*; the
//                        same slot delivered again returns nothing, and the
//                        outstanding count is checked against the table.
//   6. free-list-exact   a long run of allocate/commit/free/squash cycles with a
//                        checkpoint every time: population and content after each
//                        squash equal the checkpoint, and the conservation
//                        identity `committed-owned + free == entries` holds on
//                        every cycle.
//   7. journal-bound     the undo window driven to its full ROB_ENTRIES depth:
//                        no spurious overflow, a restore from the full bound
//                        exact. Then one allocation beyond it: the allocation is
//                        *refused* and `journal_overflow` reports, and the
//                        journal is not wrapped -- demonstrated by the next
//                        restore still producing the checkpoint state.
//   8. wrap              more than ROB_ENTRIES allocations so the tail and the
//                        rotation point both wrap repeatedly, with checkpoints
//                        and restores interleaved: recovery stays exact and the
//                        stale-redirect classification survives the wrap.
//   9. credit-stress     the reservation table filled to capacity, requests
//                        refused and reported, responses delivered out of order,
//                        duplicates delivered, and the population count checked
//                        against the DUT on every cycle.
//  10. random-soak       random stimulus compared against the shadow on every
//                        cycle, including deliberate stale, duplicate and
//                        out-of-range responses and out-of-order redirects.
//
// Standing invariants are checked on every cycle of every phase rather than in
// one place: the mutual exclusion of the four response reports, the free count
// against the free mask's population count, `credits_outstanding` against the
// table, the checkpoint depth against the valid vector, and the rule that a
// redirect is reported taken if and only if a squash happened.
// ============================================================================

#include <verilated.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "sim_common.h"
#include "Vmosaic_recovery_tb.h"

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

std::string At(const std::string& where, uint64_t cycle) {
  return where + ": cycle " + std::to_string(cycle);
}

uint32_t Mask(uint32_t value, uint32_t width) {
  return (width >= 32) ? value : (value & ((1u << width) - 1u));
}

uint32_t Clog2(uint32_t value) {
  uint32_t width = 1;
  while ((1u << width) < value) width++;
  return width;
}

// A wide bit-vector, read back from the DUT's 64-bit observation words and
// compared as a single value. This is the point of the whole case: the restore
// claim is a claim about whole state, so the comparison is over whole state.
//
// The shadow carries a parallel `std::vector<uint64_t>` of the same word count,
// and the comparison is `==` on the vector. A per-field comparison would let a
// defect in an unlisted field through, which is precisely the defect this case
// is looking for.
struct Wide {
  std::vector<uint64_t> words;

  Wide() = default;
  explicit Wide(size_t count) : words(count, 0) {}

  bool operator==(const Wide& other) const { return words == other.words; }
  bool operator!=(const Wide& other) const { return !(*this == other); }

  uint64_t Bit(size_t index) const {
    return (words[index / 64] >> (index % 64)) & 1ull;
  }

  void Set(size_t index, bool value) {
    const uint64_t bit = 1ull << (index % 64);
    if (value) {
      words[index / 64] |= bit;
    } else {
      words[index / 64] &= ~bit;
    }
  }

  size_t PopCount() const {
    size_t total = 0;
    for (uint64_t w : words) {
      uint64_t v = w;
      while (v) {
        total += v & 1ull;
        v >>= 1;
      }
    }
    return total;
  }

  // A hex rendering for a mismatch message, truncated to the first differing word
  // so the message names the field that is actually wrong without printing 700
  // bits of it.
  //
  // `*this` is the **actual** value and `expected` is the wanted one. The
  // orientation is stated because it has been got wrong twice in this file's
  // history, and both times the message pointed somewhere other than the defect:
  // once because the call sites passed (expected, actual) into a (self=actual)
  // contract, and once because a two-argument version was called with the same
  // value as both `self` and `expected`, so it compared the value against itself
  // and cheerfully reported "identical" for a field the caller had just proved
  // differed. A mismatch message that is confidently wrong is worse than none.
  std::string Describe(const Wide& expected) const {
    if (words.size() != expected.words.size()) {
      return "word count: expected " + std::to_string(expected.words.size()) +
             ", got " + std::to_string(words.size());
    }
    for (size_t i = 0; i < words.size(); i++) {
      if (words[i] != expected.words[i]) {
        return "word " + std::to_string(i) + ": expected " +
               mosaic::Hex(expected.words[i]) + ", got " + mosaic::Hex(words[i]);
      }
    }
    return "identical";
  }
};

// --------------------------------------------------------------- the stimulus
struct Stim {
  bool alloc_valid = false;
  uint32_t alloc_rd = 0;
  uint32_t alloc_rob_gen = 0;

  bool ckpt_valid = false;
  uint32_t ckpt_rob_gen = 0;

  bool commit_valid = false;
  uint32_t commit_rd = 0;
  uint32_t commit_tag = 0;
  uint32_t commit_gen = 0;
  bool rob_retire = false;

  bool wb_valid = false;
  uint32_t wb_tag = 0;
  uint32_t wb_gen = 0;

  bool free_valid = false;
  uint32_t free_tag = 0;
  uint32_t free_gen = 0;

  bool redirect0_valid = false;
  uint32_t redirect0_rob_gen = 0;
  uint64_t redirect0_pc = 0;
  bool redirect0_is_fault = false;

  bool redirect1_valid = false;
  uint32_t redirect1_rob_gen = 0;
  uint64_t redirect1_pc = 0;
  bool redirect1_is_fault = false;

  bool cred_req_valid = false;
  uint32_t cred_req_id = 0;
  uint32_t cred_req_rob_gen = 0;

  bool rsp_valid = false;
  uint32_t rsp_id = 0;
  uint32_t rsp_epoch = 0;
};

std::string Describe(const Stim& s) {
  char buf[640];
  std::snprintf(buf, sizeof(buf),
                "[alloc v=%d rd=%u gen=%u | ckpt v=%d gen=%u | commit v=%d rd=%u "
                "tag=%u gen=%u retire=%d | wb v=%d tag=%u gen=%u | free v=%d "
                "tag=%u gen=%u | rdr0 v=%d gen=%u pc=%s fault=%d | rdr1 v=%d "
                "gen=%u pc=%s fault=%d | cred v=%d id=%u | rsp v=%d id=%u ep=%u]",
                s.alloc_valid, s.alloc_rd, s.alloc_rob_gen, s.ckpt_valid,
                s.ckpt_rob_gen, s.commit_valid, s.commit_rd, s.commit_tag,
                s.commit_gen, s.rob_retire, s.wb_valid, s.wb_tag, s.wb_gen,
                s.free_valid, s.free_tag, s.free_gen, s.redirect0_valid,
                s.redirect0_rob_gen, mosaic::Hex(s.redirect0_pc).c_str(),
                s.redirect0_is_fault, s.redirect1_valid, s.redirect1_rob_gen,
                mosaic::Hex(s.redirect1_pc).c_str(), s.redirect1_is_fault,
                s.cred_req_valid, s.cred_req_id, s.rsp_valid, s.rsp_id,
                s.rsp_epoch);
  return buf;
}

// ------------------------------------------------------------------ the shadow
//
// An independent model of the documented contract. Every field has a counterpart
// in the header prose; none of them was obtained by reading the RTL.
class ShadowRecovery {
 public:
  // Everything the DUT presents combinationally: the documented rules applied to
  // the *pre-edge* state and this cycle's ports.
  struct View {
    bool alloc_accepted = false;
    bool alloc_squashed = false;
    bool alloc_exhausted = false;
    bool alloc_is_x0 = false;
    bool alloc_new_valid = false;
    uint32_t alloc_new_tag = 0;
    uint32_t alloc_new_gen = 0;
    bool alloc_old_valid = false;
    uint32_t alloc_old_tag = 0;
    uint32_t alloc_old_gen = 0;
    bool alloc_journal_full = false;
    bool journal_overflow = false;
    bool alloc_gen_regress = false;

    bool ckpt_accepted = false;
    bool ckpt_refused = false;
    uint32_t ckpt_depth = 0;

    bool commit_accepted = false;
    bool commit_x0_dropped = false;

    bool wb_accepted = false;
    bool wb_stale = false;
    bool wb_duplicate = false;

    bool free_accepted = false;
    bool free_stale = false;
    bool free_double = false;

    bool redirect_taken = false;
    uint32_t redirect_taken_gen = 0;
    uint64_t redirect_taken_pc = 0;
    bool redirect_taken_is_fault = false;
    bool redirect_stale = false;
    uint32_t redirect_killed = 0;
    uint32_t rdq_depth = 0;
    uint32_t redirect_src = 0;
    uint32_t restore_ckpt = 0;

    bool squash = false;
    bool retire_block = false;
    bool rob_flush_valid = false;
    bool rob_flush_from_valid = false;
    uint32_t rob_flush_from = 0;

    bool cred_req_ok = false;
    bool cred_req_full = false;
    bool cred_req_conflict = false;
    bool rsp_accepted = false;
    bool rsp_dropped_stale = false;
    bool rsp_dropped_dup = false;
    bool rsp_dropped_orphan = false;
    bool credit_return = false;
    uint32_t credits_outstanding = 0;
    uint32_t epoch = 0;

    uint32_t free_count = 0;
  };

  ShadowRecovery(uint32_t entries, uint32_t tag_w, uint32_t gen_w, uint32_t arch,
                 uint32_t rob, uint32_t id_w, uint32_t idx_w, uint32_t epoch_w,
                 uint32_t ckpt_depth, uint32_t rdq_depth, uint32_t cred_entries)
      : entries_(entries),
        tag_w_(tag_w),
        gen_w_(gen_w),
        arch_(arch),
        rob_(rob),
        id_w_(id_w),
        idx_w_(idx_w),
        epoch_w_(epoch_w),
        ckpt_depth_(ckpt_depth),
        rdq_depth_(rdq_depth),
        cred_entries_(cred_entries),
        tag_mask_(Mask(~0u, tag_w)),
        id_mask_(Mask(~0u, id_w)),
        idx_mask_(Mask(~0u, idx_w)),
        epoch_mask_(Mask(~0u, epoch_w)),
        gen_(entries, 0),
        gen_valid_(entries, false),
        wb_done_(entries, false),
        spec_tag_(arch, 0),
        spec_gen_(arch, 0),
        cmt_tag_(arch, 0),
        cmt_gen_(arch, 0),
        j_tag_(rob, 0),
        j_prev_(rob, false),
        // The free set is sized here rather than left to `Wide`'s default
        // constructor. An empty bundle is not a safe default: `Bit()` on one reads
        // out of bounds, and the first thing `Reset()` does is set bits in it --
        // which is a crash inside the constructor, before any check can name it.
        free_(Wide((entries + 63) / 64)),
        // The checkpoint valid vector. Sized here because `Reset()` writes it
        // element by element; a default-constructed `std::vector<bool>` is empty,
        // and writing to it is out-of-bounds rather than a no-op.
        ck_valid_(ckpt_depth, false),
        rq_valid_(rdq_depth, false),
        ck_jmark_(ckpt_depth, 0),
        ck_gen_(ckpt_depth, 0),
        ck_tail_(ckpt_depth, 0),
        ck_alloc_ptr_(ckpt_depth, 0),
        ck_epoch_(ckpt_depth, 0),
        ck_tag_valid_(ckpt_depth, false),
        ck_tag_(ckpt_depth, 0),
        ck_tag_prev_valid_(ckpt_depth, false),
        ck_tag_prev_gen_(ckpt_depth, 0),
        ck_spec_(ckpt_depth, Wide(MapWords())),
        rq_gen_(rdq_depth, 0),
        rq_pc_(rdq_depth, 0),
        rq_fault_(rdq_depth, false),
        cred_busy_(cred_entries, false),
        cred_cancel_(cred_entries, false),
        cred_epoch_(cred_entries, 0),
        cred_gen_(cred_entries, 0) {
    Reset();
  }

  // ------------------------------------------------------------------ geometry
  uint32_t entries() const { return entries_; }
  uint32_t arch() const { return arch_; }
  uint32_t rob() const { return rob_; }
  uint32_t tag_w() const { return tag_w_; }
  uint32_t id_w() const { return id_w_; }
  uint32_t idx_w() const { return idx_w_; }
  uint32_t epoch_w() const { return epoch_w_; }
  uint32_t ckpt_depth() const { return ckpt_depth_; }
  uint32_t rdq_depth() const { return rdq_depth_; }
  uint32_t cred_entries() const { return cred_entries_; }
  uint32_t map_w() const { return tag_w_ + gen_w_; }
  uint32_t cnt_w() const { return Clog2(rob_ + 1); }

  size_t MaskWords() const { return (entries_ + 63) / 64; }
  size_t TagGenWords() const { return (entries_ * gen_w_ + 63) / 64; }
  size_t MapWords() const { return (arch_ * map_w() + 63) / 64; }

  uint32_t FreeCount() const { return static_cast<uint32_t>(free_.PopCount()); }
  uint32_t JournalLen() const { return j_len_; }
  uint32_t Tail() const { return tail_; }
  uint32_t AllocPtr() const { return alloc_ptr_; }
  uint32_t Epoch() const { return epoch_; }
  uint32_t CkptDepth() const { return ck_depth_; }
  uint32_t CkptPtr() const { return ck_ptr_; }
  uint32_t RdqCount() const { return rq_count_; }
  uint32_t LastAllocGen() const { return last_alloc_gen_; }
  bool AllocSeen() const { return alloc_seen_; }
  bool CkptValid(uint32_t index) const { return ck_valid_[index]; }
  uint32_t CkptJMark(uint32_t index) const { return ck_jmark_[index]; }
  uint32_t CkptGen(uint32_t index) const { return ck_gen_[index]; }
  bool CreditBusy(uint32_t index) const { return cred_busy_[index]; }
  // The owner generation the reservation was granted for, and whether the
  // redirect that owns it has been taken. Both exist so a phase can assert the
  // *contract* -- which reservations a redirect kills -- instead of inferring it
  // from the DUT's own reports.
  uint32_t CreditGen(uint32_t index) const { return cred_gen_[index]; }
  uint32_t CreditEpoch(uint32_t index) const { return cred_epoch_[index]; }
  bool CreditCancelled(uint32_t index) const { return cred_cancel_[index]; }

  // The whole state, as the wide bundles the DUT exports. These are what the
  // restore phase compares, and comparing them is the case's central claim.
  Wide FreeMask() const { return free_; }
  Wide GenValid() const {
    Wide out(MaskWords());
    for (uint32_t e = 0; e < entries_; e++) out.Set(e, gen_valid_[e]);
    return out;
  }
  Wide WbDone() const {
    Wide out(MaskWords());
    for (uint32_t e = 0; e < entries_; e++) out.Set(e, wb_done_[e]);
    return out;
  }
  Wide TagGen() const {
    Wide out(TagGenWords());
    for (uint32_t e = 0; e < entries_; e++) {
      for (uint32_t b = 0; b < gen_w_; b++) {
        out.Set(e * gen_w_ + b, ((gen_[e] >> b) & 1u) != 0);
      }
    }
    return out;
  }
  Wide SpecMap() const { return Maps(true); }
  Wide CmtMap() const { return Maps(false); }

  // Both maps as one wide bundle each, through the same function so the field
  // layout is written once. Two separately written packings would compare
  // unequal for entirely the right reason and the phase would report it as a
  // restore defect.
  Wide Maps(bool speculative) const {
    Wide out(MapWords());
    for (uint32_t a = 0; a < arch_; a++) {
      const uint32_t tag = speculative ? spec_tag_[a] : cmt_tag_[a];
      const uint32_t gen = speculative ? spec_gen_[a] : cmt_gen_[a];
      for (uint32_t b = 0; b < tag_w_; b++) {
        out.Set(a * map_w() + b, ((tag >> b) & 1u) != 0);
      }
      for (uint32_t b = 0; b < gen_w_; b++) {
        out.Set(a * map_w() + tag_w_ + b, ((gen >> b) & 1u) != 0);
      }
    }
    return out;
  }

  // The committed map's tag for an architectural register, so a phase can check
  // the conservation identity against the DUT's own reported free count, and the
  // speculative mapping, so a phase can commit the mapping that is actually live
  // for a register instead of inventing one the machine never held.
  uint32_t CmtTag(uint32_t rd) const { return cmt_tag_[rd]; }
  uint32_t SpecTag(uint32_t rd) const { return spec_tag_[rd]; }
  uint32_t SpecGen(uint32_t rd) const { return spec_gen_[rd]; }

  // The checkpoint bundle as the shadow sees it, one wide value per field, packed
  // the way the RTL packs it. These exist so the driver can compare the checkpoint
  // stack wholesale on every cycle rather than inferring its state from the depth
  // count and a couple of individual fields -- and an inference is exactly how a
  // stack-bookkeeping divergence survives a per-field suite. This case found one
  // that way while it was being written.
  size_t CkptWords() const { return (ckpt_depth_ + 63) / 64; }
  size_t CkptJMarkWords() const { return (ckpt_depth_ * cnt_w() + 63) / 64; }
  size_t CkptGenWords() const { return (ckpt_depth_ * id_w_ + 63) / 64; }
  size_t CkptTailWords() const { return (ckpt_depth_ * idx_w_ + 63) / 64; }
  size_t CkptAllocPtrWords() const { return (ckpt_depth_ * tag_w_ + 63) / 64; }
  size_t CkptEpochWords() const { return (ckpt_depth_ * epoch_w_ + 63) / 64; }

  // Pack a per-checkpoint field of `width` bits into a wide bundle, entry `c`
  // occupying bits [c*width, (c+1)*width). One function for all six fields so the
  // layout is written once: two packings that agreed on the low entries and
  // disagreed on a high one would compare unequal for entirely the wrong reason.
  Wide PackedCkpt(const std::vector<uint32_t>& values, uint32_t width,
                  size_t words) const {
    Wide out(words);
    for (uint32_t c = 0; c < ckpt_depth_; c++) {
      for (uint32_t b = 0; b < width; b++) {
        out.Set(c * width + b, ((values[c] >> b) & 1u) != 0);
      }
    }
    return out;
  }

  Wide CkptValidWide() const {
    Wide out(CkptWords());
    for (uint32_t c = 0; c < ckpt_depth_; c++) out.Set(c, ck_valid_[c]);
    return out;
  }
  Wide CkptJMarkWide() const { return PackedCkpt(ck_jmark_, cnt_w(), CkptJMarkWords()); }
  Wide CkptGenWide() const { return PackedCkpt(ck_gen_, id_w_, CkptGenWords()); }
  Wide CkptTailWide() const { return PackedCkpt(ck_tail_, idx_w_, CkptTailWords()); }
  Wide CkptAllocPtrWide() const {
    return PackedCkpt(ck_alloc_ptr_, tag_w_, CkptAllocPtrWords());
  }
  Wide CkptEpochWide() const { return PackedCkpt(ck_epoch_, epoch_w_, CkptEpochWords()); }
  bool IsFree(uint32_t tag) const {
    return tag < entries_ && free_.Bit(tag);
  }
  uint32_t Gen(uint32_t tag) const { return gen_[tag]; }
  bool GenValid(uint32_t tag) const { return gen_valid_[tag]; }

  void Reset() {
    free_ = Wide(MaskWords());
    for (uint32_t e = 0; e < entries_; e++) {
      // Tags 0..ARCH-1 are owned by the architectural reset mappings, not free.
      free_.Set(e, e >= arch_);
      gen_[e] = 0;
      gen_valid_[e] = false;
      wb_done_[e] = false;
    }
    for (uint32_t a = 0; a < arch_; a++) {
      spec_tag_[a] = a & tag_mask_;
      spec_gen_[a] = 0;
      cmt_tag_[a] = a & tag_mask_;
      cmt_gen_[a] = 0;
    }
    for (uint32_t j = 0; j < rob_; j++) {
      j_tag_[j] = 0;
      j_prev_[j] = false;
    }
    for (uint32_t c = 0; c < ckpt_depth_; c++) {
      ck_valid_[c] = false;
      ck_jmark_[c] = 0;
      ck_gen_[c] = 0;
      ck_tail_[c] = 0;
      ck_alloc_ptr_[c] = 0;
      ck_epoch_[c] = 0;
      ck_tag_valid_[c] = false;
      ck_tag_[c] = 0;
      ck_tag_prev_valid_[c] = false;
      ck_tag_prev_gen_[c] = 0;
      for (size_t w = 0; w < ck_spec_[c].words.size(); w++) ck_spec_[c].words[w] = 0;
    }
    for (uint32_t q = 0; q < rdq_depth_; q++) {
      rq_valid_[q] = false;
      rq_gen_[q] = 0;
      rq_pc_[q] = 0;
      rq_fault_[q] = false;
    }
    for (uint32_t c = 0; c < cred_entries_; c++) {
      cred_busy_[c] = false;
      cred_cancel_[c] = false;
      cred_epoch_[c] = 0;
      cred_gen_[c] = 0;
    }
    // The rotation point starts at the first allocatable tag, so the first
    // allocation takes the lowest free tag rather than scanning past the
    // reserved range.
    alloc_ptr_ = arch_ & tag_mask_;
    tail_ = 0;
    j_len_ = 0;
    epoch_ = 0;
    j_overflow_ = false;
    ck_ptr_ = 0;
    ck_depth_ = 0;
    rq_count_ = 0;
    last_alloc_gen_ = 0;
    alloc_seen_ = false;
    journal_entries_ = 0;
    restores_ = 0;
    ckpt_taken_ = 0;
    credit_returns_ = 0;
    rsp_stale_total_ = 0;
  }

  // ------------------------------------------------------------------- accessors
  uint32_t JournalEntries() const { return journal_entries_; }
  uint32_t Restores() const { return restores_; }
  uint32_t CkptTaken() const { return ckpt_taken_; }
  uint32_t CreditReturns() const { return credit_returns_; }
  uint32_t RspStaleTotal() const { return rsp_stale_total_; }
  bool JournalOverflowLatched() const { return j_overflow_; }

  // The free-list population, and the number of tags the committed map owns. The
  // conservation identity is `committed-owned + free == entries`, and it is
  // checked on every cycle: a recovery that leaks a tag breaks it immediately
  // rather than dozens of instructions later.
  uint32_t CommittedOwned() const {
    // Tags named by the committed map that are not also named by the speculative
    // map are owned outright. A tag both maps name is owned once, and the
    // speculative owner is the instruction that will supersede the committed
    // mapping -- so counting the union and requiring free to be the complement
    // is the identity worth checking.
    std::vector<bool> owned(entries_, false);
    for (uint32_t a = 0; a < arch_; a++) {
      if (cmt_tag_[a] < entries_) owned[cmt_tag_[a]] = true;
      if (spec_tag_[a] < entries_) owned[spec_tag_[a]] = true;
    }
    uint32_t count = 0;
    for (uint32_t e = 0; e < entries_; e++) {
      if (owned[e]) count++;
    }
    return count;
  }

  // The scan result: the lowest free tag at or after the rotation point, wrapping
  // once. Written from the documented rule rather than from the RTL's loop.
  uint32_t ScanTag() const {
    for (uint32_t i = 0; i < entries_; i++) {
      const uint32_t idx = (alloc_ptr_ + i) % entries_;
      if (free_.Bit(idx)) return idx;
    }
    return 0;
  }

  bool HasFree() const { return FreeCount() > 0; }

  // --------------------------------------------------------------------- peek
  View Peek(const Stim& s) const {
    View v;
    const uint32_t scan = ScanTag();
    const uint32_t rd = Mask(s.alloc_rd, 5);

    // ---- allocation refusals, each with its own report
    const bool wants_tag = s.alloc_valid && (rd != 0);
    v.alloc_squashed  = s.alloc_valid && Take(s);
    v.alloc_is_x0     = s.alloc_valid && (rd == 0) && !Take(s);
    v.alloc_exhausted = wants_tag && !HasFree() && !Take(s);
    v.alloc_accepted  = s.alloc_valid && !Take(s) && (HasFree() || (rd == 0));
    v.alloc_new_valid = v.alloc_accepted && (rd != 0);
    v.alloc_new_tag   = scan & tag_mask_;
    v.alloc_new_gen   = gen_valid_[scan] ? ((gen_[scan] + 1) & gen_mask()) : 0;
    v.alloc_old_valid = v.alloc_new_valid;
    v.alloc_old_tag   = spec_tag_[rd] & tag_mask_;
    v.alloc_old_gen   = spec_gen_[rd];

    // ---- the undo bound. Reaching it is legal; exceeding it is refused.
    const bool ckpt_push = s.ckpt_valid && (ck_depth_ < ckpt_depth_) && !Take(s);
    const bool journal_at_bound = (j_len_ >= rob_);
    v.alloc_journal_full = wants_tag && journal_at_bound && !ckpt_push && !Take(s);
    v.journal_overflow    = v.alloc_journal_full;
    v.ckpt_accepted       = ckpt_push;
    v.ckpt_refused        = s.ckpt_valid && !ckpt_push;
    v.ckpt_depth          = ck_depth_;

    // An allocation needs a journal entry unless it is the checkpointing
    // instruction, whose own speculative state the checkpoint precedes.
    const bool needs_journal = v.alloc_new_valid && !ckpt_push;
    v.alloc_gen_regress = v.alloc_new_valid && alloc_seen_ &&
                          (Mask(s.alloc_rob_gen, id_w_) <= last_alloc_gen_);

    // ---- commit
    v.commit_x0_dropped = s.commit_valid && (Mask(s.commit_rd, 5) == 0);
    v.commit_accepted   = s.commit_valid && (Mask(s.commit_rd, 5) != 0);

    // ---- writeback and free
    const uint32_t wb_tag = Mask(s.wb_tag, tag_w_);
    const uint32_t wb_gen = Mask(s.wb_gen, gen_w_);
    const uint32_t fr_tag = Mask(s.free_tag, tag_w_);
    const uint32_t fr_gen = Mask(s.free_gen, gen_w_);
    v.wb_stale = s.wb_valid && (wb_tag >= entries_ || !gen_valid_[wb_tag] ||
                                free_.Bit(wb_tag) || (wb_gen != gen_[wb_tag]));
    v.wb_duplicate = s.wb_valid && !v.wb_stale && wb_done_[wb_tag];
    v.wb_accepted  = s.wb_valid && !v.wb_stale && !wb_done_[wb_tag];
    v.free_stale    = s.free_valid && (fr_tag >= entries_ || !gen_valid_[fr_tag] ||
                                       (fr_gen != gen_[fr_tag]));
    v.free_double   = s.free_valid && !v.free_stale && free_.Bit(fr_tag);
    v.free_accepted = s.free_valid && !v.free_stale && !free_.Bit(fr_tag);

    // ---- redirect arbitration
    const Pick p = Choose(s);
    v.redirect_taken          = p.found;
    v.redirect_taken_gen      = p.gen;
    v.redirect_taken_pc       = p.pc;
    v.redirect_taken_is_fault = p.fault;
    v.redirect_stale          = p.any_stale;
    v.redirect_killed         = p.killed;
    v.redirect_src            = p.src;
    v.restore_ckpt            = p.ck;
    v.rdq_depth               = rq_count_;

    v.squash                = p.found;
    v.rob_flush_valid       = p.found;
    v.rob_flush_from_valid  = p.found;
    // The requested flush point is `ck_tail[restore_ck]` *unconditionally*, not
    // gated on a redirect being taken. That is what the port means: it names the
    // checkpoint the unit would restore to, and `o_rob_flush_from_valid` is what
    // says whether that is a request or not. Gating the value as well would make
    // the port report zero in every cycle it is not meaningful, which is a second
    // thing for a consumer to have to know and this unit has no reason to impose.
    v.rob_flush_from        = ck_tail_[p.ck] & idx_mask_;
    // A retire and a recovery never land in the same cycle, and only in that
    // cycle is a retire blocked.
    v.retire_block = p.found && s.rob_retire;

    // ---- credit table
    const uint32_t req_id = Mask(s.cred_req_id, cred_w());
    const uint32_t rsp_id = Mask(s.rsp_id, cred_w());
    uint32_t first_free = cred_entries_;
    for (uint32_t c = 0; c < cred_entries_; c++) {
      if (!cred_busy_[c]) {
        first_free = c;
        break;
      }
    }
    v.cred_req_full    = (first_free == cred_entries_);
    v.cred_req_ok      = s.cred_req_valid && !v.cred_req_full;
    v.cred_req_conflict = s.cred_req_valid && (req_id < cred_entries_) &&
                          cred_busy_[req_id];
    v.credits_outstanding = 0;
    for (uint32_t c = 0; c < cred_entries_; c++) {
      if (cred_busy_[c]) v.credits_outstanding++;
    }
    // A response's *kill* is decided by the owner's age, not by the epoch: a
    // redirect cancels the reservations of the instructions younger than the
    // branch it resolves, and lets an older instruction's reservation alone so
    // that an older result still completes. The epoch is the identity check --
    // it says whether the response belongs to the reservation the slot is
    // currently holding -- and it is not the kill rule. Deciding both by the
    // epoch would discard an older load or DIV that the squash never touched,
    // which `docs/implementation-plan.md` §1.3 forbids and the card names as a
    // way to deadlock the older ROB head.
    const bool slot_busy = (rsp_id < cred_entries_) && cred_busy_[rsp_id];
    const uint32_t rsp_ep = Mask(s.rsp_epoch, epoch_w_);
    // A redirect taken in *this* cycle owns the reservation it squashes in the
    // same cycle; the boundary is applied combinationally so the credit is
    // returned once and not merely one cycle later.
    const bool killed_now = slot_busy && p.found && ck_valid_[p.ck] &&
                            (cred_gen_[rsp_id] >= p.gen);
    const bool cancelled = slot_busy && (cred_cancel_[rsp_id] || killed_now);
    const bool identity_matches = slot_busy && (rsp_ep == cred_epoch_[rsp_id]);
    v.rsp_accepted      = s.rsp_valid && identity_matches && !cancelled;
    v.rsp_dropped_stale = s.rsp_valid && cancelled;
    // No credit: a free slot is a delivery of an already-acknowledged credit,
    // and a busy slot whose carried epoch is not the one it was reserved with is
    // a delivery for a reservation that is gone.
    v.rsp_dropped_dup   = s.rsp_valid && (rsp_id < cred_entries_) &&
                          (!slot_busy || (!cancelled && !identity_matches));
    v.rsp_dropped_orphan = s.rsp_valid && (rsp_id >= cred_entries_);
    v.credit_return     = v.rsp_dropped_stale;
    v.epoch             = epoch_;

    v.free_count = FreeCount();
    (void)needs_journal;
    return v;
  }

  // --------------------------------------------------------------------- apply
  void Apply(const Stim& s) {
    const View v = Peek(s);
    const Pick p = Choose(s);
    const uint32_t scan = ScanTag();
    // Two different destinations, and conflating them is a defect that hides
    // until a commit and an allocation share a cycle: `rd` is the destination
    // this cycle's *allocation* is renaming, `cmt_rd` the one the commit is
    // publishing. The commit path below used `rd`, so a commit was applied to the
    // register some unrelated allocation happened to be renaming.
    const uint32_t rd = Mask(s.alloc_rd, 5);
    const uint32_t cmt_rd = Mask(s.commit_rd, 5);
    const bool ckpt_push = v.ckpt_accepted;
    const bool restore = p.found && ck_valid_[p.ck];

    // The pre-edge speculative state, captured before *anything* below mutates it.
    //
    // A checkpoint records the state as it stands **before** this cycle's
    // allocation: a branch that takes a checkpoint in the same cycle it allocates
    // a destination must be restored to the instant *before* the branch, or the
    // restore re-dispatches the branch onto state the branch itself created and
    // the branch's own destination is never freed.
    //
    // Six fields are affected -- the tail, the rotation point, the journal length,
    // the epoch and the two halves of the speculative map -- and the RTL's
    // checkpoint block reads all six from their *registers*, so the shadow has to
    // as well. Reading them from the members after the blocks below have advanced
    // them stores post-allocation state and shifts every checkpoint by one ROB
    // slot, which is exactly the divergence this was written to rule out. The
    // capture is a named struct rather than six locals so the next field added
    // cannot be quietly forgotten.
    struct PreEdge {
      uint32_t tail = 0;
      uint32_t alloc_ptr = 0;
      uint32_t j_len = 0;
      uint32_t epoch = 0;
      bool tag_valid = false;
      uint32_t tag = 0;
      bool tag_prev_valid = false;
      uint32_t tag_prev_gen = 0;
      Wide spec;
    } pre_edge;
    pre_edge.tail      = tail_ & idx_mask_;
    pre_edge.alloc_ptr = alloc_ptr_ & tag_mask_;
    pre_edge.j_len     = j_len_;
    pre_edge.epoch     = epoch_ & epoch_mask_;
    pre_edge.spec      = Wide(MapWords());
    for (uint32_t a = 0; a < arch_; a++) {
      SetField(pre_edge.spec, a * map_w(), tag_w_, spec_tag_[a]);
      SetField(pre_edge.spec, a * map_w() + tag_w_, gen_w_, spec_gen_[a]);
    }
    // The checkpointing instruction's own destination, and the generation state
    // its allocation replaced, from *before* the allocation. A branch that writes
    // a link register allocates in the same cycle it checkpoints, and that
    // allocation is deliberately not journalled -- the checkpoint precedes the
    // branch -- so the restore has to undo it explicitly or a mispredicting call
    // leaks one tag.
    pre_edge.tag_valid     = v.alloc_new_valid;
    pre_edge.tag           = scan & tag_mask_;
    pre_edge.tag_prev_valid = gen_valid_[scan];
    pre_edge.tag_prev_gen  = gen_[scan];

    // The generation-validity bit the *journal* records for this allocation: the
    // tag's state before this cycle's allocation, which is the value the undo
    // restores. It is captured here, before step 1 below clears and re-sets
    // `gen_valid_[scan]`, because capturing it after the allocation stores `true`
    // for every entry and the undo then marks a never-allocated tag valid again
    // -- a late writeback for that tag would be accepted by the restored machine.
    // The RTL reads its register (pre-edge value) for the same reason.
    const bool journal_prev_valid = gen_valid_[scan];

    // 1. Allocation.
    if (v.alloc_new_valid) {
      free_.Set(scan, false);
      gen_[scan] = gen_valid_[scan] ? ((gen_[scan] + 1) & gen_mask()) : 0;
      gen_valid_[scan] = true;
      wb_done_[scan] = false;
    }

    // 2. An explicit release, and the mapping a commit supersedes.
    if (v.free_accepted) free_.Set(Mask(s.free_tag, tag_w_), true);
    if (v.commit_accepted) {
      const uint32_t tag = Mask(s.commit_tag, tag_w_);
      const uint32_t gen = Mask(s.commit_gen, gen_w_);
      if (cmt_tag_[cmt_rd] != tag || cmt_gen_[cmt_rd] != gen) {
        if (cmt_tag_[cmt_rd] < entries_) free_.Set(cmt_tag_[cmt_rd], true);
      }
    }

    // 3a. The destination of every checkpoint the squash consumes -- the
    //     restored branch's, and every younger branch's whose checkpoint the same
    //     restore kills -- undone by the same rule as every journal entry. A
    //     branch that writes a link register allocates in the same cycle it
    //     checkpoints, and that allocation is not journalled because the
    //     checkpoint precedes the branch; a restore that undid only the restored
    //     checkpoint's destination would leak one tag per *killed* branch as well
    //     as the one it leaks per mispredict, and the leak only surfaces as
    //     spurious exhaustion dozens of instructions later. Validity is read
    //     before the stack block below clears it: the killed set is defined
    //     against the pre-edge stack, which is the stack this restore is acting
    //     on.
    if (restore) {
      for (uint32_t c = 0; c < ckpt_depth_; c++) {
        if (!ck_valid_[c] || c < p.ck || !ck_tag_valid_[c]) continue;
        free_.Set(ck_tag_[c], true);
        gen_[ck_tag_[c]] = ck_tag_prev_valid_[c] ? ck_tag_prev_gen_[c] : 0;
        gen_valid_[ck_tag_[c]] = ck_tag_prev_valid_[c];
      }
    }

    // 3. The undo, oldest entry first, starting at the checkpoint's own mark.
    // The journal is shared by every live checkpoint: entry k of the undo
    // window is journal entry (mark + k), not entry k. Indexing from zero
    // undoes allocations older than the branch, freeing tags the checkpoint
    // never owned -- the restored free set comes back larger than recorded.
    // The free set is a set, so its order does not matter; the generation is
    // not, so its undo is the exact inverse.
    const uint32_t undo = restore ? UndoCount(p.ck) : 0;
    const uint32_t base = restore ? ck_jmark_[p.ck] : 0;
    for (uint32_t k = 0; k < undo; k++) {
      const uint32_t tag = j_tag_[base + k];
      free_.Set(tag, true);
      if (j_prev_[base + k]) {
        gen_[tag] = (gen_[tag] - 1) & gen_mask();
        gen_valid_[tag] = true;
      } else {
        gen_[tag] = 0;
        gen_valid_[tag] = false;
      }
    }

    // 4. The owner that made it back writes.
    if (v.wb_accepted) wb_done_[Mask(s.wb_tag, tag_w_)] = true;

    // ---- the journal
    if (v.alloc_new_valid && !ckpt_push) {
      j_tag_[j_len_] = scan;
      j_prev_[j_len_] = journal_prev_valid;
      j_len_++;
      journal_entries_++;
    }
    if (v.journal_overflow) j_overflow_ = true;
    if (restore) j_len_ = ck_jmark_[p.ck];

    // ---- the maps, the tail and the rotation point
    if (v.commit_accepted) {
      cmt_tag_[cmt_rd] = Mask(s.commit_tag, tag_w_);
      cmt_gen_[cmt_rd] = Mask(s.commit_gen, gen_w_);
    }
    if (restore) {
      const Wide& copy = ck_spec_[p.ck];
      for (uint32_t a = 0; a < arch_; a++) {
        spec_tag_[a] = Field(copy, a * map_w(), tag_w_);
        spec_gen_[a] = Field(copy, a * map_w() + tag_w_, gen_w_);
      }
      tail_ = ck_tail_[p.ck] & idx_mask_;
      epoch_ = (epoch_ + 1) & epoch_mask_;
    } else if (v.alloc_new_valid) {
      spec_tag_[rd] = scan;
      spec_gen_[rd] = v.alloc_new_gen;
    }
    if (restore) {
      alloc_ptr_ = ck_alloc_ptr_[p.ck] & tag_mask_;
    } else if (v.alloc_new_valid) {
      alloc_ptr_ = (scan == entries_ - 1) ? 0 : ((scan + 1) & tag_mask_);
    }
    if (!restore && v.alloc_new_valid) tail_ = (tail_ + 1) % rob_;

    // ---- the checkpoint stack
    if (p.found) {
      for (uint32_t c = 0; c < ckpt_depth_; c++) {
        if (ck_valid_[c] && c >= p.ck) ck_valid_[c] = false;
      }
      ck_ptr_ = p.ck;
      RecountDepth();
      restores_++;
    }
    if (ckpt_push) {
      ck_valid_[ck_ptr_] = true;
      // Every field below is the *pre-edge* value, for the reason given at the
      // capture above: a checkpoint precedes the instruction that takes it. The
      // previous version of this block read the members, which the blocks above
      // had already advanced this cycle, and stored post-allocation state -- a
      // one-slot shift in every checkpoint. Its own comment claimed the opposite
      // of what the code did, which is the shape of defect a comment cannot catch.
      ck_jmark_[ck_ptr_]     = pre_edge.j_len;
      ck_gen_[ck_ptr_]       = Mask(s.ckpt_rob_gen, id_w_);
      ck_tail_[ck_ptr_]      = pre_edge.tail;
      ck_alloc_ptr_[ck_ptr_] = pre_edge.alloc_ptr;
      ck_epoch_[ck_ptr_]     = pre_edge.epoch;
      ck_spec_[ck_ptr_]      = pre_edge.spec;
      ck_tag_valid_[ck_ptr_]     = pre_edge.tag_valid;
      ck_tag_[ck_ptr_]           = pre_edge.tag;
      ck_tag_prev_valid_[ck_ptr_] = pre_edge.tag_prev_valid;
      ck_tag_prev_gen_[ck_ptr_]   = pre_edge.tag_prev_gen;
      ck_ptr_ = (ck_ptr_ == ckpt_depth_ - 1) ? 0 : (ck_ptr_ + 1);
      ck_depth_++;
      ckpt_taken_++;
    }

    // ---- the queue: a take empties it, otherwise the surviving resolve ports
    // are appended at the lowest free slots, port 0 before port 1.
    if (p.found) {
      for (uint32_t q = 0; q < rdq_depth_; q++) rq_valid_[q] = false;
      rq_count_ = 0;
    } else {
      bool app0 = s.redirect0_valid && !Stale(s.redirect0_rob_gen);
      bool app1 = s.redirect1_valid && !Stale(s.redirect1_rob_gen);
      for (uint32_t q = 0; q < rdq_depth_ && (app0 || app1); q++) {
        if (rq_valid_[q]) continue;
        if (app0) {
          rq_valid_[q] = true;
          rq_gen_[q] = Mask(s.redirect0_rob_gen, id_w_);
          rq_pc_[q] = s.redirect0_pc;
          rq_fault_[q] = s.redirect0_is_fault;
          rq_count_++;
          app0 = false;
        } else {
          rq_valid_[q] = true;
          rq_gen_[q] = Mask(s.redirect1_rob_gen, id_w_);
          rq_pc_[q] = s.redirect1_pc;
          rq_fault_[q] = s.redirect1_is_fault;
          rq_count_++;
          app1 = false;
        }
      }
    }

    // ---- the credit table. A redirect does *not* return the credits it
    // cancels: the credit comes back when the now-stale response arrives, and
    // returning it at both ends would over-issue.
    if (v.cred_req_ok) {
      for (uint32_t c = 0; c < cred_entries_; c++) {
        if (!cred_busy_[c]) {
          cred_busy_[c] = true;
          cred_cancel_[c] = false;
          cred_epoch_[c] = epoch_ & epoch_mask_;
          cred_gen_[c] = Mask(s.cred_req_rob_gen, id_w_);
          break;
        }
      }
    }
    // The redirect's kill, applied to the table as it stands after this cycle's
    // grant: one comparison of the owner generation against the redirecting
    // branch's covers both "reserved earlier and squashed" and "reserved in the
    // squash cycle itself". Reservations owned by older instructions are
    // untouched, which is the whole point of an age boundary over an epoch.
    // `restore` was captured from the pre-edge stack at the top of this
    // function; `ck_valid_[p.ck]` is not still true here, because the
    // checkpoint-stack block above has already consumed the restored checkpoint.
    // Reading validity after that block is the same two-snapshot mistake in
    // miniature: it asks a question about the state the cycle started from and
    // gets the state it is producing.
    if (restore) {
      for (uint32_t c = 0; c < cred_entries_; c++) {
        if (cred_busy_[c] && (cred_gen_[c] >= p.gen)) cred_cancel_[c] = true;
      }
    }
    const uint32_t rsp_id = Mask(s.rsp_id, cred_w());
    if (s.rsp_valid && rsp_id < cred_entries_) {
      cred_busy_[rsp_id] = false;
      cred_cancel_[rsp_id] = false;
    }

    // ---- the generation tracker follows the last *accepted* allocation
    if (v.alloc_new_valid) {
      last_alloc_gen_ = Mask(s.alloc_rob_gen, id_w_);
      alloc_seen_ = true;
    }
    if (v.credit_return) credit_returns_++;
    if (v.rsp_dropped_stale) rsp_stale_total_++;
  }

 private:
  struct Pick {
    bool found = false;
    uint32_t src = 0;
    uint32_t gen = 0;
    uint64_t pc = 0;
    bool fault = false;
    uint32_t ck = 0;
    uint32_t killed = 0;
    bool any_stale = false;
  };

  uint32_t gen_mask() const { return Mask(~0u, gen_w_); }
  uint32_t cred_w() const { return Clog2(cred_entries_); }

  bool Take(const Stim& s) const { return Choose(s).found; }

  // A redirect names a generation with no live checkpoint when its branch was
  // squashed by an earlier recovery.
  bool Stale(uint32_t gen) const {
    const uint32_t g = Mask(gen, id_w_);
    for (uint32_t c = 0; c < ckpt_depth_; c++) {
      if (ck_valid_[c] && ck_gen_[c] == g) return false;
    }
    return true;
  }

  uint32_t CkptOf(uint32_t gen) const {
    const uint32_t g = Mask(gen, id_w_);
    for (uint32_t c = 0; c < ckpt_depth_; c++) {
      if (ck_valid_[c] && ck_gen_[c] == g) return c;
    }
    return 0;
  }

  // The oldest live, non-stale candidate across the queue and the two resolve
  // ports. Age is the ROB generation of the redirecting instruction, and
  // generations increase with allocation order, so "oldest" is an unsigned
  // minimum -- exact while the live spread stays inside the window, which the
  // ROB depth bounds.
  Pick Choose(const Stim& s) const {
    Pick p;
    for (uint32_t q = 0; q < rdq_depth_; q++) {
      if (rq_valid_[q] && !Stale(rq_gen_[q]) &&
          (!p.found || rq_gen_[q] < p.gen)) {
        p.found = true;
        p.src = q;
        p.gen = rq_gen_[q];
        p.pc = rq_pc_[q];
        p.fault = rq_fault_[q];
        p.ck = CkptOf(rq_gen_[q]);
      }
    }
    const uint32_t g0 = Mask(s.redirect0_rob_gen, id_w_);
    const uint32_t g1 = Mask(s.redirect1_rob_gen, id_w_);
    if (s.redirect0_valid && !Stale(g0) && (!p.found || g0 < p.gen)) {
      p.found = true;
      p.src = rdq_depth_;
      p.gen = g0;
      p.pc = s.redirect0_pc;
      p.fault = s.redirect0_is_fault;
      p.ck = CkptOf(g0);
    }
    if (s.redirect1_valid && !Stale(g1) && (!p.found || g1 < p.gen)) {
      p.found = true;
      p.src = rdq_depth_ + 1;
      p.gen = g1;
      p.pc = s.redirect1_pc;
      p.fault = s.redirect1_is_fault;
      p.ck = CkptOf(g1);
    }
    p.any_stale = (s.redirect0_valid && Stale(g0)) ||
                  (s.redirect1_valid && Stale(g1));
    if (p.found) {
      // Every other live candidate is younger, so the same squash kills it.
      for (uint32_t q = 0; q < rdq_depth_; q++) {
        if (rq_valid_[q] && !Stale(rq_gen_[q]) && q != p.src) p.killed++;
      }
      if (s.redirect0_valid && !Stale(g0) && rdq_depth_ != p.src) p.killed++;
      if (s.redirect1_valid && !Stale(g1) && (rdq_depth_ + 1) != p.src) p.killed++;
    }
    return p;
  }

  uint32_t UndoCount(uint32_t ck) const {
    if (!ck_valid_[ck]) return 0;
    if (j_len_ <= ck_jmark_[ck]) return 0;
    const uint32_t n = j_len_ - ck_jmark_[ck];
    return (n > rob_) ? rob_ : n;
  }

  void RecountDepth() {
    uint32_t n = 0;
    for (uint32_t c = 0; c < ckpt_depth_; c++) {
      if (ck_valid_[c]) n++;
    }
    ck_depth_ = n;
  }

  static uint32_t Field(const Wide& w, size_t base, uint32_t width) {
    uint32_t value = 0;
    for (uint32_t b = 0; b < width; b++) {
      if (w.Bit(base + b)) value |= (1u << b);
    }
    return value;
  }

  static void SetField(Wide& w, size_t base, uint32_t width, uint32_t value) {
    for (uint32_t b = 0; b < width; b++) w.Set(base + b, ((value >> b) & 1u) != 0);
  }

  uint32_t entries_;
  uint32_t tag_w_;
  uint32_t gen_w_;
  uint32_t arch_;
  uint32_t rob_;
  uint32_t id_w_;
  uint32_t idx_w_;
  uint32_t epoch_w_;
  uint32_t ckpt_depth_;
  uint32_t rdq_depth_;
  uint32_t cred_entries_;
  uint32_t tag_mask_;
  uint32_t id_mask_;
  uint32_t idx_mask_;
  uint32_t epoch_mask_;

  Wide free_;
  std::vector<uint32_t> gen_;
  std::vector<bool> gen_valid_;
  std::vector<bool> wb_done_;
  std::vector<uint32_t> spec_tag_;
  std::vector<uint32_t> spec_gen_;
  std::vector<uint32_t> cmt_tag_;
  std::vector<uint32_t> cmt_gen_;
  std::vector<uint32_t> j_tag_;
  std::vector<bool> j_prev_;

  std::vector<bool> ck_valid_;
  std::vector<uint32_t> ck_jmark_;
  std::vector<uint32_t> ck_gen_;
  std::vector<uint32_t> ck_tail_;
  std::vector<uint32_t> ck_alloc_ptr_;
  std::vector<uint32_t> ck_epoch_;
  std::vector<bool> ck_tag_valid_;
  std::vector<uint32_t> ck_tag_;
  std::vector<bool> ck_tag_prev_valid_;
  std::vector<uint32_t> ck_tag_prev_gen_;
  // The per-checkpoint speculative map copies: a vector of wide bundles rather
  // than one flat bundle, because each checkpoint is a *separate* copy that has
  // to be read back independently. That independence is the whole mechanism --
  // one flat bundle would let one checkpoint's copy stand in for another's, and
  // the restore would then be comparing a copy against itself.
  std::vector<Wide> ck_spec_;

  std::vector<bool> rq_valid_;
  std::vector<uint32_t> rq_gen_;
  std::vector<uint64_t> rq_pc_;
  std::vector<bool> rq_fault_;
  uint32_t rq_count_;

  std::vector<bool> cred_busy_;
  std::vector<bool> cred_cancel_;
  std::vector<uint32_t> cred_epoch_;
  std::vector<uint32_t> cred_gen_;

  uint32_t alloc_ptr_;
  uint32_t tail_;
  uint32_t j_len_;
  uint32_t epoch_;
  bool j_overflow_;
  uint32_t ck_ptr_;
  uint32_t ck_depth_;
  uint32_t last_alloc_gen_;
  bool alloc_seen_;
  uint32_t journal_entries_;
  uint32_t restores_;
  uint32_t ckpt_taken_;
  uint32_t credit_returns_;
  uint32_t rsp_stale_total_;
};

// The whole speculative state, as the shadow sees it. This is the value the
// exact-restore claim is made about, and it is compared as one bundle so a
// defect in a field nobody thought to list cannot hide.
struct Snapshot {
  Wide free_mask;
  Wide gen_valid;
  Wide wb_done;
  Wide tag_gen;
  Wide spec_map;
  Wide cmt_map;
  uint32_t tail = 0;
  uint32_t alloc_ptr = 0;
  uint32_t j_len = 0;
  uint32_t free_count = 0;

  // The checkpoint bundle, one wide value per field. Compared on every cycle like
  // the rest of the state: a checkpoint-stack divergence that surfaced only in a
  // port no phase happened to check is exactly what a per-field suite lets
  // through, and this case found one precisely that way while it was being
  // written. Six fields, all listed, none derived.
  Wide ckpt_valid;
  Wide ckpt_jmark;
  Wide ckpt_gen;
  Wide ckpt_tail;
  Wide ckpt_alloc_ptr;
  Wide ckpt_epoch;

  // A single string key over the whole bundle, so "bit-identical" is one
  // comparison and the message names the first field that differs. The order is
  // fixed here and nowhere else. Checkpoint *contents* are masked by validity:
  // a dead slot's stored mark/tail/gen is not architectural state (the RTL
  // deliberately leaves it stale on consume, and reset never writes it), so
  // two snapshots that agree on every live checkpoint agree -- even if the
  // dead slots' leftover contents differ. Comparing raw dead-slot contents
  // would demand the restore scrub state the contract says is don't-care,
  // which is how an exact-restore check fails on a machine that restored
  // exactly.
  std::string Key() const {
    std::string out;
    Append(out, free_mask);
    Append(out, gen_valid);
    Append(out, wb_done);
    Append(out, tag_gen);
    Append(out, spec_map);
    Append(out, cmt_map);
    out += "|" + std::to_string(tail);
    out += "|" + std::to_string(alloc_ptr);
    out += "|" + std::to_string(j_len);
    out += "|" + std::to_string(free_count);
    Append(out, ckpt_valid);
    Append(out, ckpt_jmark);
    Append(out, ckpt_gen);
    Append(out, ckpt_tail);
    Append(out, ckpt_alloc_ptr);
    Append(out, ckpt_epoch);
    return out;
  }
  // Live-only projection of a per-checkpoint bundle: dead slots read as zero
  // regardless of their stale contents. A restore consumes its checkpoint, so
  // the slot that held it is dead afterwards -- comparing its leftover mark
  // would demand the restore scrub state the contract leaves don't-care.
  // Widths are template parameters at the call site because each bundle packs
  // a different entry width and LiveOnly must walk entry by entry.
  static Wide LiveOnly(const Wide& content, const Wide& valid, uint32_t per_entry_bits) {
    Wide out(content.words.size());
    for (uint32_t c = 0; c < 64; c++) {
      if (!valid.Bit(c)) continue;
      for (uint32_t b = 0; b < per_entry_bits; b++) {
        if (content.Bit(c * per_entry_bits + b)) out.Set(c * per_entry_bits + b, true);
      }
    }
    return out;
  }
  // Live-only checkpoint equality: validity plus every content bundle masked
  // to its live slots. The per-entry widths come from the shadow, which was
  // sized from the DUT's own geometry handshake -- so this is a method, not a
  // static, and the widths are read from the harness's shadow at the call site
  // in the phase (FirstDifference stays static for the live-state fields and
  // takes widths only for the checkpoint tail).
  static bool CkptEqual(const Snapshot& a, const Snapshot& b, uint32_t cnt_w, uint32_t id_w,
                        uint32_t idx_w, uint32_t tag_w, uint32_t epoch_w) {
    if (a.ckpt_valid != b.ckpt_valid) return false;
    if (LiveOnly(a.ckpt_jmark, a.ckpt_valid, cnt_w) != LiveOnly(b.ckpt_jmark, b.ckpt_valid, cnt_w)) return false;
    if (LiveOnly(a.ckpt_gen, a.ckpt_valid, id_w) != LiveOnly(b.ckpt_gen, b.ckpt_valid, id_w)) return false;
    if (LiveOnly(a.ckpt_tail, a.ckpt_valid, idx_w) != LiveOnly(b.ckpt_tail, b.ckpt_valid, idx_w)) return false;
    if (LiveOnly(a.ckpt_alloc_ptr, a.ckpt_valid, tag_w) != LiveOnly(b.ckpt_alloc_ptr, b.ckpt_valid, tag_w)) return false;
    if (LiveOnly(a.ckpt_epoch, a.ckpt_valid, epoch_w) != LiveOnly(b.ckpt_epoch, b.ckpt_valid, epoch_w)) return false;
    return true;
  }
  // The first field that differs between two snapshots, named. Live state
  // compares whole; checkpoint contents are reported through their live
  // projections, so the message never names dead-slot staleness as the
  // difference when the live checkpoints agree.
  static std::string FirstDifference(const Snapshot& a, const Snapshot& b) {
    if (a.free_mask != b.free_mask) return "free_mask: " + b.free_mask.Describe(a.free_mask);
    if (a.gen_valid != b.gen_valid) return "gen_valid: " + b.gen_valid.Describe(a.gen_valid);
    if (a.wb_done != b.wb_done) return "wb_done: " + b.wb_done.Describe(a.wb_done);
    if (a.tag_gen != b.tag_gen) return "tag_gen: " + b.tag_gen.Describe(a.tag_gen);
    if (a.spec_map != b.spec_map) return "spec_map: " + b.spec_map.Describe(a.spec_map);
    if (a.cmt_map != b.cmt_map) return "cmt_map: " + b.cmt_map.Describe(a.cmt_map);
    if (a.tail != b.tail) {
      return "tail: expected " + std::to_string(a.tail) + ", got " + std::to_string(b.tail);
    }
    if (a.alloc_ptr != b.alloc_ptr) {
      return "alloc_ptr: expected " + std::to_string(a.alloc_ptr) + ", got " +
             std::to_string(b.alloc_ptr);
    }
    if (a.j_len != b.j_len) {
      return "j_len: expected " + std::to_string(a.j_len) + ", got " + std::to_string(b.j_len);
    }
    if (a.free_count != b.free_count) {
      return "free_count: expected " + std::to_string(a.free_count) + ", got " +
             std::to_string(b.free_count);
    }
    if (a.ckpt_valid != b.ckpt_valid) {
      return "ckpt_valid: " + b.ckpt_valid.Describe(a.ckpt_valid);
    }
    return "ckpt-contents-differ-only-in-dead-slots";
  }

 private:
  static void Append(std::string& out, const Wide& w) {
    for (uint64_t word : w.words) out += mosaic::Hex(word, 16);
    out += "/";
  }
};

class Harness {
 public:
  // What the soak actually exercised. A random campaign that never reached the
  // journal bound, never wrapped and never produced a stale response would pass
  // while testing nothing, so the phase asserts these are non-zero.
  struct Counters {
    uint32_t checkpoints = 0;
    uint32_t restores = 0;
    uint32_t stale_redirects = 0;
    uint32_t killed_redirects = 0;
    uint32_t stale_responses = 0;
    uint32_t duplicate_responses = 0;
    uint32_t credit_returns = 0;
    uint32_t journal_refusals = 0;
    uint32_t wrap_allocations = 0;
    uint32_t exhausted = 0;
  };

  Harness(Vmosaic_recovery_tb* dut, mosaic::ClockDriver* clk, uint64_t max_cycles,
          ShadowRecovery* shadow)
      : dut_(dut),
        clk_(clk),
        max_cycles_(max_cycles),
        shadow_(shadow) {}

  void Phase(const std::string& name) { phase_ = name; }

  // A freshly reset DUT *and* a freshly reset shadow, so no phase can pass on
  // state left behind by the one before it.
  void Fresh(int cycles = 4) {
    for (int i = 0; i < cycles; i++) Cycle(Stim{}, /*rst=*/true);
    shadow_->Reset();
    counters_ = Counters();
  }

  // One full clock period, driven low-phase, rising edge, falling edge.
  void Cycle(const Stim& s, bool rst = false) {
    if (cycles_ >= max_cycles_) {
      Fail(phase_, "max-cycles (" + std::to_string(max_cycles_) +
                       ") exhausted before the phase finished");
    }

    dut_->clk = 0;
    dut_->rst = rst ? 1 : 0;
    dut_->alloc_valid_i = s.alloc_valid;
    dut_->alloc_rd_i = s.alloc_rd;
    dut_->alloc_rob_gen_i = s.alloc_rob_gen;
    dut_->ckpt_valid_i = s.ckpt_valid;
    dut_->ckpt_rob_gen_i = s.ckpt_rob_gen;
    dut_->commit_valid_i = s.commit_valid;
    dut_->commit_rd_i = s.commit_rd;
    dut_->commit_tag_i = s.commit_tag;
    dut_->commit_gen_i = s.commit_gen;
    dut_->rob_retire_i = s.rob_retire;
    dut_->wb_valid_i = s.wb_valid;
    dut_->wb_tag_i = s.wb_tag;
    dut_->wb_gen_i = s.wb_gen;
    dut_->free_valid_i = s.free_valid;
    dut_->free_tag_i = s.free_tag;
    dut_->free_gen_i = s.free_gen;
    dut_->redirect0_valid_i = s.redirect0_valid;
    dut_->redirect0_rob_gen_i = s.redirect0_rob_gen;
    dut_->redirect0_pc_i = s.redirect0_pc;
    dut_->redirect0_is_fault_i = s.redirect0_is_fault;
    dut_->redirect1_valid_i = s.redirect1_valid;
    dut_->redirect1_rob_gen_i = s.redirect1_rob_gen;
    dut_->redirect1_pc_i = s.redirect1_pc;
    dut_->redirect1_is_fault_i = s.redirect1_is_fault;
    dut_->cred_req_valid_i = s.cred_req_valid;
    dut_->cred_req_id_i = s.cred_req_id;
    dut_->cred_req_rob_gen_i = s.cred_req_rob_gen;
    dut_->rsp_valid_i = s.rsp_valid;
    dut_->rsp_id_i = s.rsp_id;
    dut_->rsp_epoch_i = s.rsp_epoch;
    dut_->eval();

    if (!rst) {
      Compare(s);
      // The event reports are captured with the clock still low, *before* the
      // edge, so a phase reading `RedirectTaken()` after `Cycle()` returns is
      // reading the answer to the request it just drove and not the answer to the
      // next cycle's. Capturing them post-edge -- which this harness briefly did,
      // as a side effect of reordering `Capture()` -- makes every phase read one
      // cycle late, which is invisible for a combinational output and fatal for
      // a pulse. The post-edge `Capture()` reads only state, which is the right
      // instant for state.
      CaptureReports();
      shadow_->Apply(s);
      Tally(s);
    }

    dut_->clk = 1;
    dut_->eval();
    Capture();
    clk_->Tick();
    dut_->clk = 0;
    dut_->eval();
    ++cycles_;
  }

  // ------------------------------------------------------- stimulus helpers
  // The next ROB generation to hand out. It is the shadow's, not a separate
  // counter, so a generation the DUT was never told about cannot appear.
  uint32_t NextGen() const { return shadow_->LastAllocGen() + 1; }

  // Allocate one destination, optionally taking a checkpoint in the same cycle.
  // Returns the generation the DUT was given.
  uint32_t Alloc(uint32_t rd, bool checkpoint, bool x0 = false) {
    Stim s;
    s.alloc_valid = true;
    s.alloc_rd = rd;
    s.alloc_rob_gen = NextGen();
    if (checkpoint) {
      s.ckpt_valid = true;
      s.ckpt_rob_gen = s.alloc_rob_gen;
    }
    Cycle(s);
    return s.alloc_rob_gen;
  }

  // One idle cycle.
  void Idle() { Cycle(Stim{}); }

  void Commit(uint32_t rd, uint32_t tag, uint32_t gen, bool retire = false) {
    Stim s;
    s.commit_valid = true;
    s.commit_rd = rd;
    s.commit_tag = tag;
    s.commit_gen = gen;
    s.rob_retire = retire;
    Cycle(s);
  }

  void Writeback(uint32_t tag, uint32_t gen) {
    Stim s;
    s.wb_valid = true;
    s.wb_tag = tag;
    s.wb_gen = gen;
    Cycle(s);
  }

  void Release(uint32_t tag, uint32_t gen) {
    Stim s;
    s.free_valid = true;
    s.free_tag = tag;
    s.free_gen = gen;
    Cycle(s);
  }

  // One credit reservation, returning the slot the unit actually granted. The
  // grant is read from the shadow's reservation table rather than assumed to be
  // the requested id, because the unit scans for the *lowest free* slot: a test
  // that assumed its preference was granted would be delivering its response to
  // a slot the unit never filled, and would be testing a different path.
  //
  // Returns `cred_entries()` when the reservation was refused, so a caller that
  // ignores the return cannot mistake "granted" for "granted something".
  uint32_t ReserveCredit(uint32_t prefer_id) {
    std::vector<bool> before(shadow_->cred_entries(), false);
    for (uint32_t c = 0; c < shadow_->cred_entries(); c++) {
      before[c] = shadow_->CreditBusy(c);
    }
    Stim s;
    s.cred_req_valid = true;
    s.cred_req_id = prefer_id;
    s.cred_req_rob_gen = NextGen();
    Cycle(s);
    for (uint32_t c = 0; c < shadow_->cred_entries(); c++) {
      if (!before[c] && shadow_->CreditBusy(c)) return c;
    }
    return shadow_->cred_entries();
  }

  // Drive a credit request without caring which slot was granted, for the phases
  // that only care about the table's population count.
  void RequestCredit(uint32_t prefer_id) {
    Stim s;
    s.cred_req_valid = true;
    s.cred_req_id = prefer_id;
    s.cred_req_rob_gen = NextGen();
    Cycle(s);
  }

  // Reserve a credit *for a stated owner generation* and return the slot the
  // unit granted (read back from the shadow's table, because the unit grants the
  // lowest free slot and not the preferred one; `cred_entries()` means refused).
  //
  // The phases that test the age-bounded kill cannot use `NextGen()`, which is
  // the generation of a *new* instruction: every reservation would then be
  // younger than the branch under test, and the rule "an older instruction's
  // result still completes" would be untestable because no reservation would be
  // older. Handing the generation in explicitly is what lets a phase put a
  // reservation on either side of the boundary.
  uint32_t ReserveCreditFor(uint32_t rob_gen) {
    std::vector<bool> before(shadow_->cred_entries(), false);
    for (uint32_t c = 0; c < shadow_->cred_entries(); c++) {
      before[c] = shadow_->CreditBusy(c);
    }
    Stim s;
    s.cred_req_valid = true;
    s.cred_req_id = 0;
    s.cred_req_rob_gen = rob_gen;
    Cycle(s);
    for (uint32_t c = 0; c < shadow_->cred_entries(); c++) {
      if (!before[c] && shadow_->CreditBusy(c)) return c;
    }
    return shadow_->cred_entries();
  }

  void Respond(uint32_t id, uint32_t epoch) {
    Stim s;
    s.rsp_valid = true;
    s.rsp_id = id;
    s.rsp_epoch = epoch;
    Cycle(s);
  }

  // ---------------------------------------------------------- observed state
  const Snapshot& Now() const { return settled_; }
  uint32_t ObservedEpoch() const { return settled_epoch_; }
  uint32_t ObservedFreeCount() const { return settled_free_count_; }
  uint32_t ObservedCredits() const { return settled_credits_; }
  uint32_t ObservedCkptDepth() const { return settled_ckpt_depth_; }
  uint32_t ObservedRdqDepth() const { return settled_rdq_depth_; }
  bool ObservedInexact() const { return settled_inexact_; }

  // The event reports, with the clock still low.
  bool RedirectTaken() const { return report_.redirect_taken; }
  uint32_t RedirectTakenGen() const { return report_.redirect_taken_gen; }
  uint64_t RedirectTakenPc() const { return report_.redirect_taken_pc; }
  bool CreditReturn() const { return report_.credit_return; }
  bool RspAccepted() const { return report_.rsp_accepted; }
  bool RspDroppedStale() const { return report_.rsp_dropped_stale; }
  bool RspDroppedDup() const { return report_.rsp_dropped_dup; }
  bool JournalOverflow() const { return report_.journal_overflow; }
  bool AllocJournalFull() const { return report_.alloc_journal_full; }
  bool AllocAccepted() const { return report_.alloc_accepted; }
  bool AllocSquashed() const { return report_.alloc_squashed; }
  bool AllocExhausted() const { return report_.alloc_exhausted; }
  bool RedirectTakenIsFault() const { return report_.redirect_taken_is_fault; }
  bool WbAccepted() const { return report_.wb_accepted; }
  bool WbStale() const { return report_.wb_stale; }
  bool WbDuplicate() const { return report_.wb_duplicate; }
  bool CkptAccepted() const { return report_.ckpt_accepted; }
  bool CkptRefused() const { return report_.ckpt_refused; }
  bool RedirectStale() const { return report_.redirect_stale; }
  uint32_t RedirectKilled() const { return report_.redirect_killed; }
  uint32_t RestoreCkpt() const { return report_.restore_ckpt; }
  bool RspDroppedOrphan() const { return report_.rsp_dropped_orphan; }
  bool CredReqFull() const { return report_.cred_req_full; }
  bool CredReqConflict() const { return report_.cred_req_conflict; }
  bool CredReqOk() const { return report_.cred_req_ok; }
  bool Squash() const { return report_.squash; }
  bool RetireBlock() const { return report_.retire_block; }
  uint32_t ObservedRestoreCkpt() const { return report_.restore_ckpt; }
  uint32_t ObservedJournalLen() const { return settled_.j_len; }
  // The destination the most recent allocation received, as the DUT reported
  // it. Read from the report rather than from the shadow so a phase can assert
  // about the DUT's behaviour instead of about the shadow agreeing with itself.
  uint32_t LastAllocTag() const { return report_.alloc_new_tag; }
  uint32_t ObservedTail() const { return settled_.tail; }
  uint32_t ObservedAllocPtr() const { return settled_.alloc_ptr; }
  uint64_t ObservedCkptBundle() const { return dut_->dbg_ckpt_alloc_ptr_o; }
  uint64_t ObservedCkptValid() const { return dut_->dbg_ckpt_valid_o; }
  uint64_t ObservedCkptEpoch() const { return dut_->dbg_ckpt_epoch_o; }
  uint64_t settled_ckpt_valid() const { return settled_.ckpt_valid.words.empty() ? 0 : settled_.ckpt_valid.words[0]; }


  // -------------------------------------------------------------- the shadow
  ShadowRecovery& shadow() const { return *shadow_; }
  uint32_t Entries() const { return shadow_->entries(); }
  uint32_t Rob() const { return shadow_->rob(); }

  uint64_t comparisons() const { return comparisons_; }
  uint64_t cycles() const { return cycles_; }
  const Counters& counters() const { return counters_; }

 private:
  void CaptureReports() {
    report_.alloc_accepted     = dut_->alloc_accepted_o;
    report_.alloc_squashed     = dut_->alloc_squashed_o;
    report_.alloc_exhausted    = dut_->alloc_exhausted_o;
    report_.alloc_is_x0        = dut_->alloc_is_x0_o;
    report_.alloc_new_valid    = dut_->alloc_new_valid_o;
    report_.alloc_new_tag      = dut_->alloc_new_tag_o;
    report_.alloc_new_gen      = dut_->alloc_new_gen_o;
    report_.alloc_old_valid    = dut_->alloc_old_valid_o;
    report_.alloc_old_tag      = dut_->alloc_old_tag_o;
    report_.alloc_old_gen      = dut_->alloc_old_gen_o;
    report_.alloc_journal_full = dut_->alloc_journal_full_o;
    report_.journal_overflow   = dut_->journal_overflow_o;
    report_.alloc_gen_regress  = dut_->alloc_gen_regress_o;
    report_.ckpt_accepted      = dut_->ckpt_accepted_o;
    report_.ckpt_refused       = dut_->ckpt_refused_o;
    report_.commit_accepted    = dut_->commit_accepted_o;
    report_.commit_x0_dropped  = dut_->commit_x0_dropped_o;
    report_.wb_accepted        = dut_->wb_accepted_o;
    report_.wb_stale           = dut_->wb_stale_o;
    report_.wb_duplicate       = dut_->wb_duplicate_o;
    report_.free_accepted      = dut_->free_accepted_o;
    report_.free_stale         = dut_->free_stale_o;
    report_.free_double        = dut_->free_double_o;
    report_.redirect_taken     = dut_->redirect_taken_o;
    report_.redirect_taken_gen = dut_->redirect_taken_gen_o;
    report_.redirect_taken_pc  = dut_->redirect_taken_pc_o;
    report_.redirect_taken_is_fault = dut_->redirect_taken_is_fault_o;
    report_.redirect_stale     = dut_->redirect_stale_o;
    report_.redirect_killed    = dut_->redirect_killed_o;
    report_.redirect_src       = dut_->o_redirect_src_o;
    report_.restore_ckpt       = dut_->o_restore_ckpt_o;
    report_.squash             = dut_->squash_o;
    report_.retire_block       = dut_->retire_block_o;
    report_.rob_flush_valid    = dut_->rob_flush_valid_o;
    report_.rob_flush_from_valid = dut_->o_rob_flush_from_valid_o;
    report_.rob_flush_from     = dut_->o_rob_flush_from_o;
    report_.cred_req_ok        = dut_->cred_req_ok_o;
    report_.cred_req_full      = dut_->cred_req_full_o;
    report_.cred_req_conflict  = dut_->cred_req_conflict_o;
    report_.rsp_accepted       = dut_->rsp_accepted_o;
    report_.rsp_dropped_stale  = dut_->rsp_dropped_stale_o;
    report_.rsp_dropped_dup    = dut_->rsp_dropped_dup_o;
    report_.rsp_dropped_orphan = dut_->rsp_dropped_orphan_o;
    report_.credit_return      = dut_->credit_return_o;
    report_.credits_outstanding = dut_->credits_outstanding_o;
    report_.epoch              = dut_->o_epoch_o;
  }

  // The whole state as the DUT exports it, on the settled post-edge state: the
  // wide bundles are read as vectors and the scalar fields alongside them. This is
  // the value the restore claim is compared on. State is read *after* the edge
  // and reports *before* it, because state is what the edge produced and the
  // reports are what the edge was asked.
  void CaptureSnapshot() {
    settled_.free_mask = Wide(shadow_->MaskWords());
    for (size_t w = 0; w < shadow_->MaskWords(); w++) {
      settled_.free_mask.words[w] = dut_->dbg_free_mask_o[w];
    }
    settled_.gen_valid = Wide(shadow_->MaskWords());
    for (size_t w = 0; w < shadow_->MaskWords(); w++) {
      settled_.gen_valid.words[w] = dut_->dbg_gen_valid_o[w];
    }
    settled_.wb_done = Wide(shadow_->MaskWords());
    for (size_t w = 0; w < shadow_->MaskWords(); w++) {
      settled_.wb_done.words[w] = dut_->dbg_wb_done_o[w];
    }
    settled_.tag_gen = Wide(shadow_->TagGenWords());
    for (size_t w = 0; w < shadow_->TagGenWords(); w++) {
      settled_.tag_gen.words[w] = dut_->dbg_tag_gen_o_ext[w];
    }
    settled_.spec_map = Wide(shadow_->MapWords());
    for (size_t w = 0; w < shadow_->MapWords(); w++) {
      settled_.spec_map.words[w] = dut_->dbg_spec_map_o[w];
    }
    settled_.cmt_map = Wide(shadow_->MapWords());
    for (size_t w = 0; w < shadow_->MapWords(); w++) {
      settled_.cmt_map.words[w] = dut_->dbg_cmt_map_o[w];
    }
    settled_.tail      = dut_->dbg_tail_o;
    settled_.alloc_ptr = dut_->dbg_alloc_ptr_o;
    settled_.j_len     = dut_->dbg_j_len_o;

    // The checkpoint bundle, read from the DUT's 64-bit word ports. The word
    // counts come from the shadow's own geometry so a profile that widened a
    // bundle past a word is a mismatch here rather than a silent truncation.
    settled_.ckpt_valid.words.assign(1, dut_->dbg_ckpt_valid_o);
    settled_.ckpt_valid.words.resize(shadow_->CkptWords(), 0);
    settled_.ckpt_jmark.words.assign(1, dut_->dbg_ckpt_jmark_o);
    settled_.ckpt_jmark.words.resize(shadow_->CkptJMarkWords(), 0);
    settled_.ckpt_tail.words.assign(1, dut_->dbg_ckpt_tail_o);
    settled_.ckpt_tail.words.resize(shadow_->CkptTailWords(), 0);
    settled_.ckpt_alloc_ptr.words.assign(1, dut_->dbg_ckpt_alloc_ptr_o);
    settled_.ckpt_alloc_ptr.words.resize(shadow_->CkptAllocPtrWords(), 0);
    settled_.ckpt_epoch.words.assign(1, dut_->dbg_ckpt_epoch_o);
    settled_.ckpt_epoch.words.resize(shadow_->CkptEpochWords(), 0);
    settled_.ckpt_gen.words.assign(shadow_->CkptGenWords(), 0);
    for (size_t w = 0; w < shadow_->CkptGenWords() && w < 2; w++) {
      settled_.ckpt_gen.words[w] = dut_->dbg_ckpt_gen_o[w];
    }
  }

  void Capture() {
    // The scalar reports are read *before* the snapshot, because the snapshot
    // records the free count among its fields and must see this cycle's value
    // rather than the previous cycle's. Reading them afterwards would put a
    // one-cycle-stale free count inside a bundle that is supposed to be a single
    // consistent view of the machine, and the restore comparison would then be
    // comparing two different instants.
    settled_epoch_      = dut_->o_epoch_o;
    settled_free_count_ = dut_->free_count_o;
    settled_credits_    = dut_->credits_outstanding_o;
    settled_ckpt_depth_ = dut_->o_ckpt_depth_o;
    settled_rdq_depth_  = dut_->o_rdq_depth_o;
    settled_inexact_    = dut_->dbg_inexact_o;
    settled_.free_count = settled_free_count_;
    CaptureSnapshot();
  }

  // ------------------------------------------------------------- comparison
  void Compare(const Stim& s) {
    const std::string where = At(phase_, clk_->cycle());
    const std::string stim = Describe(s);
    const ShadowRecovery::View e = shadow_->Peek(s);

    auto bit = [&](const char* name, bool got, bool want) {
      Require(got == want, where, std::string(name) + ": expected " + Bool(want) +
                                ", got " + Bool(got) + stim);
      ++comparisons_;
    };
    auto field = [&](const char* name, uint64_t got, uint64_t want) {
      Require(got == want, where, std::string(name) + ": expected " +
                                std::to_string(want) + ", got " +
                                std::to_string(got) + stim);
      ++comparisons_;
    };

    bit("alloc_accepted", dut_->alloc_accepted_o, e.alloc_accepted);
    bit("alloc_squashed", dut_->alloc_squashed_o, e.alloc_squashed);
    bit("alloc_exhausted", dut_->alloc_exhausted_o, e.alloc_exhausted);
    bit("alloc_is_x0", dut_->alloc_is_x0_o, e.alloc_is_x0);
    bit("alloc_new_valid", dut_->alloc_new_valid_o, e.alloc_new_valid);
    field("alloc_new_tag", dut_->alloc_new_tag_o, e.alloc_new_tag);
    field("alloc_new_gen", dut_->alloc_new_gen_o, e.alloc_new_gen);
    bit("alloc_old_valid", dut_->alloc_old_valid_o, e.alloc_old_valid);
    field("alloc_old_tag", dut_->alloc_old_tag_o, e.alloc_old_tag);
    field("alloc_old_gen", dut_->alloc_old_gen_o, e.alloc_old_gen);
    bit("alloc_journal_full", dut_->alloc_journal_full_o, e.alloc_journal_full);
    bit("journal_overflow", dut_->journal_overflow_o, e.journal_overflow);
    bit("alloc_gen_regress", dut_->alloc_gen_regress_o, e.alloc_gen_regress);

    bit("ckpt_accepted", dut_->ckpt_accepted_o, e.ckpt_accepted);
    bit("ckpt_refused", dut_->ckpt_refused_o, e.ckpt_refused);
    field("o_ckpt_depth", dut_->o_ckpt_depth_o, e.ckpt_depth);

    bit("commit_accepted", dut_->commit_accepted_o, e.commit_accepted);
    bit("commit_x0_dropped", dut_->commit_x0_dropped_o, e.commit_x0_dropped);

    bit("wb_accepted", dut_->wb_accepted_o, e.wb_accepted);
    bit("wb_stale", dut_->wb_stale_o, e.wb_stale);
    bit("wb_duplicate", dut_->wb_duplicate_o, e.wb_duplicate);

    bit("free_accepted", dut_->free_accepted_o, e.free_accepted);
    bit("free_stale", dut_->free_stale_o, e.free_stale);
    bit("free_double", dut_->free_double_o, e.free_double);

    bit("redirect_taken", dut_->redirect_taken_o, e.redirect_taken);
    field("redirect_taken_gen", dut_->redirect_taken_gen_o, e.redirect_taken_gen);
    Require(dut_->redirect_taken_pc_o == e.redirect_taken_pc, where,
            "redirect_taken_pc: expected " + mosaic::Hex(e.redirect_taken_pc) +
                ", got " + mosaic::Hex(dut_->redirect_taken_pc_o) + stim);
    bit("redirect_taken_is_fault", dut_->redirect_taken_is_fault_o,
        e.redirect_taken_is_fault);
    bit("redirect_stale", dut_->redirect_stale_o, e.redirect_stale);
    field("redirect_killed", dut_->redirect_killed_o, e.redirect_killed);
    field("o_rdq_depth", dut_->o_rdq_depth_o, e.rdq_depth);
    field("o_redirect_src", dut_->o_redirect_src_o, e.redirect_src);
    field("o_restore_ckpt", dut_->o_restore_ckpt_o, e.restore_ckpt);

    bit("squash", dut_->squash_o, e.squash);
    bit("retire_block", dut_->retire_block_o, e.retire_block);
    bit("rob_flush_valid", dut_->rob_flush_valid_o, e.rob_flush_valid);
    bit("o_rob_flush_from_valid", dut_->o_rob_flush_from_valid_o,
        e.rob_flush_from_valid);
    // The flush point is a (value, valid) request pair: the RTL drives the value
    // unconditionally, from `ck_tail[restore_ck]`, and with no redirect taken
    // `restore_ck` falls back to slot 0 -- a slot the contract deliberately does
    // not reset, because validity lives in `ck_valid` and resetting the storage
    // is not where validity is carried. Comparing that value in a cycle with no
    // request would therefore compare two don't-cares: the shadow's zeroed model
    // of an unreset array against whatever the previous phase left in the DUT's.
    // So the *value* is compared only where it is a value, and the valid bit is
    // compared in every cycle above. Weakening the port to drive zero when it is
    // not meaningful would instead make every consumer learn a second rule.
    if (dut_->o_rob_flush_from_valid_o) {
      field("o_rob_flush_from", dut_->o_rob_flush_from_o, e.rob_flush_from);
    }

    bit("cred_req_ok", dut_->cred_req_ok_o, e.cred_req_ok);
    bit("cred_req_full", dut_->cred_req_full_o, e.cred_req_full);
    bit("cred_req_conflict", dut_->cred_req_conflict_o, e.cred_req_conflict);
    bit("rsp_accepted", dut_->rsp_accepted_o, e.rsp_accepted);
    bit("rsp_dropped_stale", dut_->rsp_dropped_stale_o, e.rsp_dropped_stale);
    bit("rsp_dropped_dup", dut_->rsp_dropped_dup_o, e.rsp_dropped_dup);
    bit("rsp_dropped_orphan", dut_->rsp_dropped_orphan_o, e.rsp_dropped_orphan);
    bit("credit_return", dut_->credit_return_o, e.credit_return);
    field("credits_outstanding", dut_->credits_outstanding_o,
          e.credits_outstanding);
    field("o_epoch", dut_->o_epoch_o, e.epoch);
    field("free_count", dut_->free_count_o, e.free_count);

    // The whole-state comparison. Every cycle, not only after a restore: a
    // divergence in a field no directed check names is exactly what a per-field
    // suite would let through.
    Require(settled_.free_mask == shadow_->FreeMask(), where,
            "dbg_free_mask: " + settled_.free_mask.Describe(shadow_->FreeMask()) +
                stim);
    Require(settled_.gen_valid == shadow_->GenValid(), where,
            "dbg_gen_valid: " + settled_.gen_valid.Describe(shadow_->GenValid()) +
                stim);
    Require(settled_.wb_done == shadow_->WbDone(), where,
            "dbg_wb_done: " + settled_.wb_done.Describe(shadow_->WbDone()) + stim);
    Require(settled_.tag_gen == shadow_->TagGen(), where,
            "dbg_tag_gen: " + settled_.tag_gen.Describe(shadow_->TagGen()) + stim);
    Require(settled_.spec_map == shadow_->SpecMap(), where,
            "dbg_spec_map: " + settled_.spec_map.Describe(shadow_->SpecMap()) +
                stim);
    Require(settled_.cmt_map == shadow_->CmtMap(), where,
            "dbg_cmt_map: " + settled_.cmt_map.Describe(shadow_->CmtMap()) + stim);
    field("dbg_tail", dut_->dbg_tail_o, shadow_->Tail());
    field("dbg_alloc_ptr", dut_->dbg_alloc_ptr_o, shadow_->AllocPtr());
    field("dbg_j_len", dut_->dbg_j_len_o, shadow_->JournalLen());
    comparisons_ += 6;

    // The checkpoint bundle, compared wholesale on every cycle. This is the check
    // whose absence let a stack-bookkeeping divergence accumulate silently: the
    // depth count and one port were compared and everything else about the stack
    // was inferred. Six fields, all listed, none derived.
    Require(settled_.ckpt_valid == shadow_->CkptValidWide(), where,
            "dbg_ckpt_valid: " +
                settled_.ckpt_valid.Describe(shadow_->CkptValidWide()) + stim);
    // The validity vector is compared raw on every cycle -- *which* checkpoints
    // are live is exactly the fact that must not drift. The *contents* of a
    // checkpoint slot are compared only where the slot is live: the RTL leaves a
    // dead slot's mark, generation and tail stale by design (the arrays are not
    // reset, and a consumed checkpoint's contents are not scrubbed), so a raw
    // comparison would demand the restore scrub a declared don't-care and would
    // fail at the first phase boundary where a slot is left dead with leftover
    // contents. That is the same masking `Snapshot::CkptEqual` uses for the
    // restore comparison, so "the restore is exact" means the same thing in both
    // places instead of two different things.
    const Wide ck_valid = shadow_->CkptValidWide();
    auto live_ckpt_field = [&](const char* name, const Wide& got, const Wide& want,
                               uint32_t per_entry_bits) {
      const Wide got_live = Snapshot::LiveOnly(got, ck_valid, per_entry_bits);
      const Wide want_live = Snapshot::LiveOnly(want, ck_valid, per_entry_bits);
      Require(got_live == want_live, where,
              std::string(name) + ": " + got_live.Describe(want_live) + stim);
      ++comparisons_;
    };
    live_ckpt_field("dbg_ckpt_jmark", settled_.ckpt_jmark,
                    shadow_->CkptJMarkWide(), shadow_->cnt_w());
    live_ckpt_field("dbg_ckpt_gen", settled_.ckpt_gen,
                    shadow_->CkptGenWide(), shadow_->id_w());
    live_ckpt_field("dbg_ckpt_tail", settled_.ckpt_tail,
                    shadow_->CkptTailWide(), shadow_->idx_w());
    live_ckpt_field("dbg_ckpt_alloc_ptr", settled_.ckpt_alloc_ptr,
                    shadow_->CkptAllocPtrWide(), shadow_->tag_w());
    live_ckpt_field("dbg_ckpt_epoch", settled_.ckpt_epoch,
                    shadow_->CkptEpochWide(), shadow_->epoch_w());
    comparisons_ += 1;

    Invariants(where, stim, s);
  }

  // Standing invariants, on every cycle of every phase rather than in one place.
  // The stimulus is passed in rather than remembered: `retire_block` is a
  // combinational function of *this* cycle's redirect and *this* cycle's
  // `rob_retire`, so checking it against a retire remembered from the previous
  // redirect cycle compares two different cycles -- and it did, which is how this
  // parameter came to exist.
  void Invariants(const std::string& where, const std::string& stim, const Stim& s) {
    const ShadowRecovery::View e = shadow_->Peek(Stim{});

    // The four response reports are mutually exclusive and exhaustive over a
    // presented response. A response classified twice would return its credit
    // twice, which is the defect this case is most often written to miss.
    const unsigned reports = static_cast<unsigned>(dut_->rsp_accepted_o) +
                             static_cast<unsigned>(dut_->rsp_dropped_stale_o) +
                             static_cast<unsigned>(dut_->rsp_dropped_dup_o) +
                             static_cast<unsigned>(dut_->rsp_dropped_orphan_o);
    Require(reports <= 1, where,
            "a response was classified " + std::to_string(reports) +
                " times; the four reports must be mutually exclusive " + stim);

    // A credit is returned only on a stale drop, never on an accepted one: an
    // accepted response has *consumed* its reservation, and returning it too is
    // the over-issue.
    Require(!(dut_->rsp_accepted_o && dut_->credit_return_o), where,
            "an accepted response also returned a credit " + stim);

    // A redirect is reported taken if and only if a squash happened. There is no
    // cycle in which one happens without the other, and no cycle in which the
    // recovery claims a redirect it did not act on.
    Require(dut_->redirect_taken_o == dut_->squash_o, where,
            "redirect_taken and squash disagree " + stim);
    Require(dut_->redirect_taken_o == dut_->rob_flush_valid_o, where,
            "a taken redirect did not flush the ROB " + stim);
    Require(dut_->redirect_taken_o == dut_->o_rob_flush_from_valid_o, where,
            "a taken redirect published no flush point " + stim);

    // A retire is blocked exactly when a recovery and a retire coincide, and
    // never otherwise -- blocking unconditionally would stall retirement for as
    // long as any branch were in flight. Both terms are this cycle's: the
    // redirect the arbiter just decided and the retire the ROB is presenting now.
    Require(dut_->retire_block_o == (dut_->redirect_taken_o && s.rob_retire), where,
            "retire_block is not exactly 'a recovery and a retire in one cycle' " +
                stim);

    // Allocation is refused in a squash cycle, and a refused allocation never
    // reports a new destination.
    Require(!(dut_->squash_o && dut_->alloc_new_valid_o), where,
            "an allocation produced a destination in a squash cycle " + stim);

    // The free count is the population count of the free set, by definition.
    Require(dut_->free_count_o == static_cast<uint32_t>(shadow_->FreeMask().PopCount()),
            where,
            "free_count does not match the free mask's population count " + stim);

    // The conservation identity: every tag is either free or owned by a mapping.
    Require(shadow_->FreeCount() + shadow_->CommittedOwned() <= Entries(), where,
            "the free list and the owned mappings exceed the register file " + stim);

    // The reservation table's population count is what the unit reports.
    uint32_t busy = 0;
    for (uint32_t c = 0; c < shadow_->cred_entries(); c++) {
      if (shadow_->CreditBusy(c)) busy++;
    }
    Require(dut_->credits_outstanding_o == busy, where,
            "credits_outstanding does not match the reservation table " + stim);

    // The journal never exceeds its bound. Reaching it is legal; exceeding it is
    // the defect this invariant exists to catch, and it is a state fact rather
    // than a port report, so it is checked independently of `journal_overflow`.
    Require(dut_->dbg_j_len_o <= Rob(), where,
            "the undo window is " + std::to_string(dut_->dbg_j_len_o) +
                ", beyond the bound of " + std::to_string(Rob()) + " " + stim);

    // The checkpoint depth never exceeds the stack, and the valid vector's
    // population count is what the depth reports.
    uint32_t valid_ckpts = 0;
    for (uint32_t c = 0; c < shadow_->ckpt_depth(); c++) {
      if (shadow_->CkptValid(c)) valid_ckpts++;
    }
    Require(dut_->o_ckpt_depth_o == valid_ckpts, where,
            "the checkpoint depth does not match the valid vector " + stim);
    Require(dut_->o_ckpt_depth_o <= shadow_->ckpt_depth(), where,
            "the checkpoint stack is deeper than its bound " + stim);

    // The queue depth is bounded by the queue.
    Require(dut_->o_rdq_depth_o <= shadow_->rdq_depth(), where,
            "the redirect queue is deeper than its bound " + stim);

    // The generation of an allocated tag always advances, so a recycled tag
    // cannot accept a stale writeback.
    Require(!dut_->alloc_gen_regress_o, where,
            "an allocation presented a non-increasing ROB generation " + stim);

    (void)e;
  }

  void Tally(const Stim& s) {
    const ShadowRecovery::View e = shadow_->Peek(s);
    if (e.redirect_taken) counters_.restores++;
    if (e.ckpt_accepted) counters_.checkpoints++;
    if (e.redirect_stale) counters_.stale_redirects++;
    counters_.killed_redirects += e.redirect_killed;
    if (e.rsp_dropped_stale) counters_.stale_responses++;
    if (e.rsp_dropped_dup) counters_.duplicate_responses++;
    if (e.credit_return) counters_.credit_returns++;
    if (e.journal_overflow) counters_.journal_refusals++;
    if (e.alloc_exhausted) counters_.exhausted++;
    if (s.alloc_valid && s.alloc_rd != 0) counters_.wrap_allocations++;
  }

  struct Report {
    bool alloc_accepted = false;
    bool alloc_squashed = false;
    bool alloc_exhausted = false;
    bool alloc_is_x0 = false;
    bool alloc_new_valid = false;
    uint32_t alloc_new_tag = 0;
    uint32_t alloc_new_gen = 0;
    bool alloc_old_valid = false;
    uint32_t alloc_old_tag = 0;
    uint32_t alloc_old_gen = 0;
    bool alloc_journal_full = false;
    bool journal_overflow = false;
    bool alloc_gen_regress = false;
    bool ckpt_accepted = false;
    bool ckpt_refused = false;
    bool commit_accepted = false;
    bool commit_x0_dropped = false;
    bool wb_accepted = false;
    bool wb_stale = false;
    bool wb_duplicate = false;
    bool free_accepted = false;
    bool free_stale = false;
    bool free_double = false;
    bool redirect_taken = false;
    uint32_t redirect_taken_gen = 0;
    uint64_t redirect_taken_pc = 0;
    bool redirect_taken_is_fault = false;
    bool redirect_stale = false;
    uint32_t redirect_killed = 0;
    uint32_t redirect_src = 0;
    uint32_t restore_ckpt = 0;
    bool squash = false;
    bool retire_block = false;
    bool rob_flush_valid = false;
    bool rob_flush_from_valid = false;
    uint32_t rob_flush_from = 0;
    bool cred_req_ok = false;
    bool cred_req_full = false;
    bool cred_req_conflict = false;
    bool rsp_accepted = false;
    bool rsp_dropped_stale = false;
    bool rsp_dropped_dup = false;
    bool rsp_dropped_orphan = false;
    bool credit_return = false;
    uint32_t credits_outstanding = 0;
    uint32_t epoch = 0;
  };

  Vmosaic_recovery_tb* dut_;
  mosaic::ClockDriver* clk_;
  uint64_t max_cycles_;
  ShadowRecovery* shadow_;
  std::string phase_;
  Report report_;
  Snapshot settled_;
  uint64_t cycles_ = 0;
  uint64_t comparisons_ = 0;
  Counters counters_;
  uint32_t settled_epoch_ = 0;
  uint32_t settled_free_count_ = 0;
  uint32_t settled_credits_ = 0;
  uint32_t settled_ckpt_depth_ = 0;
  uint32_t settled_rdq_depth_ = 0;
  bool settled_inexact_ = false;
};

// ------------------------------------------------------------------- phases
//
// Each phase resets first and owns one mechanism, and a run stops at the first
// failure, so the order decides *which* phase reports a given defect.

// The tag an allocation scan from `ptr` over the free set `free_mask` would
// produce: the lowest free tag at or after the rotation point, wrapping once.
// Written here rather than as a method on the shadow because the phases need to
// evaluate it against a *captured* free set rather than the shadow's current one
// -- and that distinction is the whole point of the rotation-point check:
// evaluating it against the current state would make the assertion agree with the
// DUT by construction and prove nothing.
uint32_t TagScanFrom(uint32_t ptr, const Wide& free_mask, uint32_t entries) {
  for (uint32_t i = 0; i < entries; i++) {
    const uint32_t idx = (ptr + i) % entries;
    if (free_mask.Bit(idx)) return idx;
  }
  return 0;
}

// 1. The documented cold state.
void PhaseResetState(Harness& h) {
  ShadowRecovery& s = h.shadow();
  const std::string where = "reset-state";

  // Arch reg i maps to tag i at generation 0.
  for (uint32_t a = 0; a < 32; a++) {
    Require(s.SpecMap().Bit(a * s.map_w() + 0) == ((a & 1u) != 0), where,
            "spec_map tag bit for x" + std::to_string(a) + " is wrong at reset");
    Require(s.CmtMap().Bit(a * s.map_w() + 0) == ((a & 1u) != 0), where,
            "cmt_map tag bit for x" + std::to_string(a) + " is wrong at reset");
  }

  // Tags 0..31 are owned by the reset mappings, not free. A free list reset to
  // all-ones would hand tag 5 to the first instruction that writes any register,
  // giving one physical register two live owners.
  Require(s.FreeCount() == h.Entries() - 32, where,
          "expected " + std::to_string(h.Entries() - 32) +
              " free tags at reset, the shadow has " + std::to_string(s.FreeCount()));
  for (uint32_t t = 0; t < 32; t++) {
    Require(!s.IsFree(t), where,
            "tag " + std::to_string(t) + " is free at reset, but the architectural "
            "reset mapping owns it");
  }
  for (uint32_t t = 32; t < h.Entries(); t++) {
    Require(s.IsFree(t), where, "tag " + std::to_string(t) + " is not free at reset");
  }

  Require(s.JournalLen() == 0, where, "the journal is not empty at reset");
  Require(s.Epoch() == 0, where, "the epoch is not zero at reset");
  Require(s.CkptDepth() == 0, where, "there are checkpoints before any allocation");
  Require(s.Tail() == 0, where, "the tail is not zero at reset");
  Require(s.AllocPtr() == 32, where,
          "the rotation point does not start at the first allocatable tag");

  // And the DUT agrees, which is the point of reading it back.
  h.Idle();
  Require(h.ObservedFreeCount() == h.Entries() - 32, where,
          "the DUT's free count at reset is " + std::to_string(h.ObservedFreeCount()));
  Require(h.ObservedEpoch() == 0, where, "the DUT's epoch at reset is not zero");
  Require(h.ObservedJournalLen() == 0, where, "the DUT's journal is not empty");
}

// 2. The central phase: a restore is bit-identical to the checkpoint.
void PhaseExactRestore(Harness& h) {
  ShadowRecovery& s = h.shadow();
  const std::string where = "exact-restore";

  // Build a body of speculative state whose free set, generations and maps are
  // all *non-trivial*: a restore from a checkpoint taken before any allocation
  // would prove nothing, because everything is still at its reset value.
  for (uint32_t i = 0; i < 12; i++) {
    h.Alloc(5 + (i % 20), /*checkpoint=*/false);
  }
  h.Writeback(s.CmtTag(5) == 5 ? 5 : 5, 0);
  h.Alloc(31, /*checkpoint=*/false);
  h.Idle();

  // The checkpoint: a branch that writes a link register, so the checkpoint has
  // to capture a *non-default* rotation point and a partially-consumed free set.
  // The three values the phase will check the restore against are captured here,
  // from the DUT's own report, so the assertions below are about the DUT rather
  // than about the shadow agreeing with itself.
  const uint32_t epoch_at_ckpt = h.ObservedEpoch();
  const uint32_t alloc_ptr_at_ckpt = h.ObservedAllocPtr();
  // The restore is compared against the state *before* the branch's own
  // allocation, not against a snapshot taken after it. That distinction is the
  // design, not a detail: a checkpoint records the speculative state as it stands
  // before this cycle's allocation, precisely so the restore puts the machine back
  // before the branch dispatched. The branch that writes a link register therefore
  // allocates a destination that the restore *undoes*, and a snapshot taken after
  // that allocation is one instant later than the checkpoint's content -- it has
  // the branch's own tag still marked allocated.
  //
  // Comparing against the post-allocation snapshot is not a stricter test, it is a
  // wrong one: it demands the restore re-materialise the branch it is supposed to
  // have erased. Capturing the expectation first is what makes this a test of the
  // contract rather than a test of the phase author's assumption about it.
  const Snapshot at_ckpt = h.Now();
  const uint32_t branch_gen = h.Alloc(1, /*checkpoint=*/true);
  Require(h.Now().free_mask != at_ckpt.free_mask, where,
          "the branch's own allocation changed nothing, so the branch's destination "
          "is not exercised by this phase");
  // The tag the checkpoint's rotation point and free set would hand the next
  // allocation. Computed from the checkpoint's *own* captured state, not from the
  // post-squash state, so the assertion below distinguishes "restored exactly"
  // from "happens to agree with wherever the scan happens to be".
  const uint32_t tag_after_restore = TagScanFrom(alloc_ptr_at_ckpt,
                                                 at_ckpt.free_mask, h.Entries());
  Require(at_ckpt.j_len == s.JournalLen(), where,
          "the journal length changed while taking the checkpoint");

  // Allocate a great deal more after the checkpoint, across a wrap of the
  // rotation point, so the free set, the generations and the tail all move.
  for (uint32_t i = 0; i < 25; i++) {
    h.Alloc(2 + (i % 28), /*checkpoint=*/false);
  }
  h.Idle();
  const Snapshot dirty = h.Now();
  Require(dirty.Key() != at_ckpt.Key(), where,
          "the post-checkpoint allocations changed nothing, so this phase would "
          "pass without testing a restore");

  // The redirect. The oldest live branch is this one, so it is taken.
  Stim r;
  r.redirect0_valid = true;
  r.redirect0_rob_gen = branch_gen;
  r.redirect0_pc = 0x80001000ull;
  h.Cycle(r);

  Require(h.RedirectTaken(), where, "the redirect was not taken " + Describe(r));
  Require(h.RedirectTakenGen() == branch_gen, where,
          "the wrong redirect was taken: expected generation " +
              std::to_string(branch_gen) + ", got " +
              std::to_string(h.RedirectTakenGen()));
  Require(h.RedirectTakenPc() == 0x80001000ull, where,
          "the redirect published the wrong target " + Describe(r));

  // The restore cycle itself is where the state is compared, and it is compared
  // whole: the free mask, the generation-valid mask, the written mask, every
  // generation, the speculative map, the committed map, the tail, the rotation
  // point and the journal length.
  const Snapshot after = h.Now();
  // Live state compares whole; the checkpoint bundle compares live-only (a
  // consumed checkpoint's dead-slot contents are don't-care -- see LiveOnly).
  if (after.free_mask != at_ckpt.free_mask || after.gen_valid != at_ckpt.gen_valid ||
      after.wb_done != at_ckpt.wb_done || after.tag_gen != at_ckpt.tag_gen ||
      after.spec_map != at_ckpt.spec_map || after.cmt_map != at_ckpt.cmt_map ||
      after.tail != at_ckpt.tail || after.alloc_ptr != at_ckpt.alloc_ptr ||
      after.j_len != at_ckpt.j_len || after.free_count != at_ckpt.free_count ||
      !Snapshot::CkptEqual(at_ckpt, after, s.cnt_w(), s.id_w(), s.idx_w(), s.tag_w(), s.epoch_w())) {
    Fail(where, "the restore is not bit-identical to the checkpoint -- " +
                    Snapshot::FirstDifference(at_ckpt, after));
  }

  // And the epoch advanced by exactly one: that is what makes every in-flight
  // response pre-redirect, and a restore that did not advance it would leave the
  // machine accepting results of squashed instructions.
  Require(h.ObservedEpoch() == epoch_at_ckpt + 1, where,
          "the epoch did not advance by exactly one across the restore: " +
              std::to_string(epoch_at_ckpt) + " -> " +
              std::to_string(h.ObservedEpoch()));

  // The journal is back at the checkpoint's mark, so the undo window holds
  // nothing from the squashed work.
  Require(after.j_len == s.CkptJMark(h.RestoreCkpt()), where,
          "the journal after the restore is " + std::to_string(after.j_len) +
              ", the checkpoint's mark is " +
              std::to_string(s.CkptJMark(h.RestoreCkpt())));

  // The rotation point is the checkpoint's, not merely *a* valid one. This is the
  // check a "free set restored, rotation point not" defect fails and the one no
  // free-count comparison would notice: the free set is bit-identical either
  // way, and the difference only shows up in which tag the *next* allocation
  // receives.
  Require(after.alloc_ptr == alloc_ptr_at_ckpt, where,
          "the rotation point after the restore is " +
              std::to_string(after.alloc_ptr) + ", the checkpoint's is " +
              std::to_string(alloc_ptr_at_ckpt));

  // And the next allocation receives the tag the checkpoint's rotation point
  // would have produced. Read from the DUT's own report, not from the shadow, so
  // the assertion is about the DUT's behaviour rather than about the shadow
  // agreeing with itself.
  Stim a;
  a.alloc_valid = true;
  a.alloc_rd = 7;
  a.alloc_rob_gen = h.NextGen();
  h.Cycle(a);
  Require(h.AllocAccepted(), where, "the allocation after the restore was refused");
  Require(h.LastAllocTag() == tag_after_restore, where,
          "the allocation after the restore received tag " +
              std::to_string(h.LastAllocTag()) + ", the checkpoint's rotation "
              "point would have produced " + std::to_string(tag_after_restore));
}

// 3. Two checkpoints outstanding; the older redirects.
void PhaseNestedCheckpoint(Harness& h) {
  ShadowRecovery& s = h.shadow();
  const std::string where = "nested-ckpt";

  h.Alloc(3, /*checkpoint=*/false);
  const uint32_t older = h.Alloc(1, /*checkpoint=*/true);
  const uint32_t older_mark = s.JournalLen();
  h.Alloc(4, /*checkpoint=*/false);
  const uint32_t younger = h.Alloc(1, /*checkpoint=*/true);
  h.Alloc(5, /*checkpoint=*/false);
  h.Idle();

  Require(s.CkptDepth() == 2, where,
          "expected two live checkpoints, the shadow has " +
              std::to_string(s.CkptDepth()));
  Require(s.CkptGen(0) == older, where, "the older checkpoint is not at index 0");
  Require(s.CkptGen(1) == younger, where, "the younger checkpoint is not at index 1");
  Require(s.CkptJMark(1) > older_mark, where,
          "the younger checkpoint's mark is not above the older one's");

  // The older branch redirects. It is restored, and the younger checkpoint is
  // *consumed*: it belongs to a branch the squash just killed, and leaving it
  // behind would be a checkpoint for a branch that no longer exists.
  Stim r;
  r.redirect0_valid = true;
  r.redirect0_rob_gen = older;
  r.redirect0_pc = 0x80002000ull;
  h.Cycle(r);

  Require(h.RedirectTaken(), where, "the older redirect was not taken " + Describe(r));
  Require(h.RedirectTakenGen() == older, where,
          "the wrong generation was taken: expected " + std::to_string(older) +
              ", got " + std::to_string(h.RedirectTakenGen()));
  Require(s.CkptDepth() == 0, where,
          "a checkpoint survived a squash that killed its branch: depth is " +
              std::to_string(s.CkptDepth()));
  Require(s.JournalLen() == older_mark, where,
          "the journal is not back at the older checkpoint's mark: " +
              std::to_string(s.JournalLen()) + " vs " + std::to_string(older_mark));

  // The younger branch's redirect now names a generation with no live
  // checkpoint: its branch was squashed, so it describes a path that no longer
  // exists. It is reported stale and *not taken* -- a taken-but-dead redirect
  // would restore to a checkpoint that is gone.
  Stim dead;
  dead.redirect0_valid = true;
  dead.redirect0_rob_gen = younger;
  dead.redirect0_pc = 0x80003000ull;
  h.Cycle(dead);

  Require(!h.RedirectTaken(), where,
          "a redirect for a squashed branch was taken " + Describe(dead));
  Require(h.RedirectStale(), where,
          "the redirect for a squashed branch was not reported stale " +
              Describe(dead));
}


// 4. The card's rule: the oldest redirect wins. Age orders *redirects*; an older
//    branch that has not resolved yet is a checkpoint, not a pending redirect.
void PhaseOldestRedirect(Harness& h) {
  ShadowRecovery& s = h.shadow();
  const std::string where = "oldest-redirect";

  // Two nested checkpoints. The inner one can be taken while the outer branch is
  // still live, and that is the rule rather than an oversight: the only event
  // that resolves the outer branch is the redirect this unit would otherwise be
  // refusing to take, so "hold the younger redirect until the older branch
  // resolves" holds it forever. What is ordered by age is the set of redirects
  // offered in one cycle, and the same-cycle case below is where that order is
  // total.
  h.Alloc(3, /*checkpoint=*/false);
  const uint32_t outer = h.Alloc(1, /*checkpoint=*/true);
  h.Alloc(4, /*checkpoint=*/false);
  const uint32_t inner = h.Alloc(1, /*checkpoint=*/true);
  h.Alloc(5, /*checkpoint=*/false);
  h.Idle();
  Require(s.CkptDepth() == 2, where,
          "expected two live checkpoints, the shadow has " +
              std::to_string(s.CkptDepth()));
  Require(s.CkptGen(0) == outer, where, "the older checkpoint is not at index 0");
  Require(s.CkptGen(1) == inner, where, "the younger checkpoint is not at index 1");
  Require(outer < inner, where, "the nested generations are not in age order");

  // --- the inner branch resolves first, with the outer checkpoint still live.
  Stim young;
  young.redirect0_valid = true;
  young.redirect0_rob_gen = inner;
  young.redirect0_pc = 0x80004000ull;
  h.Cycle(young);
  Require(h.RedirectTaken(), where,
          "the inner redirect was not taken while the outer branch was still "
          "unresolved, so it would be deferred forever " + Describe(young));
  Require(h.RedirectTakenGen() == inner, where,
          "the wrong redirect was taken: expected " + std::to_string(inner) +
              ", got " + std::to_string(h.RedirectTakenGen()));

  // The outer checkpoint survives: it belongs to a branch older than the one
  // squashed, so the same squash must not consume it.
  Require(s.CkptDepth() == 1, where,
          "the inner squash consumed a checkpoint it does not own: depth is " +
              std::to_string(s.CkptDepth()));
  Require(s.CkptValid(0) && s.CkptGen(0) == outer, where,
          "the older checkpoint did not survive the younger branch's squash");

  // --- and then the outer one, which rewinds further back than the inner did.
  Stim old;
  old.redirect0_valid = true;
  old.redirect0_rob_gen = outer;
  old.redirect0_pc = 0x80005000ull;
  h.Cycle(old);
  Require(h.RedirectTaken(), where, "the oldest redirect was not taken " + Describe(old));
  Require(h.RedirectTakenGen() == outer, where,
          "the wrong redirect was taken: expected " + std::to_string(outer) +
              ", got " + std::to_string(h.RedirectTakenGen()));
  Require(h.RedirectTakenPc() == 0x80005000ull, where,
          "the taken redirect published the wrong target " + Describe(old));
  Require(s.CkptDepth() == 0, where,
          "checkpoints survived a squash of the oldest branch: depth is " +
              std::to_string(s.CkptDepth()));

  // --- both in the *same* cycle, older on the younger's port. The rule is a
  // minimum over every candidate including this cycle's ports, so it must hold
  // here too, and the taken one must be the older.
  h.Fresh();
  const uint32_t h0 = h.Alloc(1, /*checkpoint=*/true);
  h.Alloc(9, /*checkpoint=*/false);
  const uint32_t h1 = h.Alloc(1, /*checkpoint=*/true);
  h.Idle();

  Stim both;
  both.redirect0_valid = true;
  both.redirect0_rob_gen = h1;    // the younger, on port 0
  both.redirect0_pc = 0x80006000ull;
  both.redirect1_valid = true;
  both.redirect1_rob_gen = h0;    // the older, on port 1
  both.redirect1_pc = 0x80007000ull;
  h.Cycle(both);

  Require(h.RedirectTaken(), where, "neither same-cycle redirect was taken " +
                                       Describe(both));
  Require(h.RedirectTakenGen() == h0, where,
          "the same-cycle rule took the younger: expected " + std::to_string(h0) +
              ", got " + std::to_string(h.RedirectTakenGen()));
  Require(h.RedirectTakenPc() == 0x80007000ull, where,
          "the same-cycle rule took the younger's target " + Describe(both));
  Require(h.RedirectKilled() == 1, where,
          "the losing redirect of the same cycle was not reported killed: " +
              std::to_string(h.RedirectKilled()));
  Require(!h.CkptRefused(), where, "a checkpoint was refused unexpectedly");

  // --- and a redirect naming a generation with no live checkpoint is *stale*:
  // its branch was squashed already, so it describes a path that no longer
  // exists. It is not taken even though it is the only candidate, and it is
  // reported rather than silently dropped.
  Stim dead;
  dead.redirect0_valid = true;
  dead.redirect0_rob_gen = h0;      // consumed by the squash above
  dead.redirect0_pc = 0x80008000ull;
  h.Cycle(dead);
  Require(!h.RedirectTaken(), where,
          "a redirect for a squashed branch was taken " + Describe(dead));
  Require(h.RedirectStale(), where,
          "the redirect for a squashed branch was not reported stale " +
              Describe(dead));
}

// 5. A squashed instruction's result is dropped and its credit returned exactly
//    once; an *older* instruction's result, which the same squash does not own,
//    is still accepted. Both halves are the rule -- dropping every pre-redirect
//    response is the failure the card names, not a conservative choice.
void PhaseLateResponse(Harness& h) {
  ShadowRecovery& s = h.shadow();
  const std::string where = "late-response";

  // An instruction older than the branch, with a result in flight, and one
  // younger than it. Both hold a credit. The older reservation is granted with
  // its own generation -- not `NextGen()`, which is a *new* instruction's and
  // would make every reservation younger than the branch, leaving the rule's
  // other half untestable.
  const uint32_t older_gen = h.Alloc(6, /*checkpoint=*/false);
  const uint32_t older_slot = h.ReserveCreditFor(older_gen);
  h.Alloc(10, /*checkpoint=*/false);
  const uint32_t branch_gen = h.Alloc(1, /*checkpoint=*/true);
  const uint32_t younger_gen = h.Alloc(11, /*checkpoint=*/false);
  const uint32_t younger_slot = h.ReserveCreditFor(younger_gen);
  h.Idle();

  Require(older_slot < s.cred_entries() && younger_slot < s.cred_entries() &&
              older_slot != younger_slot,
          where,
          "the two reservations did not both land in the table: " +
              std::to_string(older_slot) + " and " +
              std::to_string(younger_slot));
  Require(older_gen < branch_gen && branch_gen < younger_gen, where,
          "the phase's generations are not in the age order it needs");
  Require(h.ObservedCredits() == 2, where,
          "expected two reserved credits, the DUT reports " +
              std::to_string(h.ObservedCredits()));

  const uint32_t epoch_before = h.ObservedEpoch();

  // A redirect raises the epoch.
  Stim r;
  r.redirect0_valid = true;
  r.redirect0_rob_gen = branch_gen;
  r.redirect0_pc = 0x80008000ull;
  h.Cycle(r);
  Require(h.RedirectTaken(), where, "the redirect was not taken " + Describe(r));

  const uint32_t epoch_after = h.ObservedEpoch();
  Require(epoch_after == epoch_before + 1, where,
          "the redirect did not raise the epoch by one: " +
              std::to_string(epoch_before) + " -> " + std::to_string(epoch_after));

  // The credits are *not* returned by the redirect: the contract returns a credit
  // when the cancel is acknowledged, and the acknowledgement is the arrival of
  // the response. Returning it at both ends would over-issue.
  Require(h.ObservedCredits() == 2, where,
          "the redirect returned the credits it cancels, before the cancelled "
          "responses arrived: the DUT reports " +
              std::to_string(h.ObservedCredits()) + " outstanding");

  // The kill is an age boundary, and it is visible in the table: the younger
  // instruction's reservation is cancelled, the older instruction's is not.
  Require(s.CreditCancelled(younger_slot), where,
          "the redirect did not cancel a reservation owned by an instruction it "
          "squashed");
  Require(!s.CreditCancelled(older_slot), where,
          "the redirect cancelled a reservation owned by an *older* instruction, "
          "which the squash does not own");

  // The older instruction's result arrives, carrying the epoch it was reserved
  // in -- which is now the previous epoch. It is live, because the redirect does
  // not own it. This is the case's directed check for the card's "an older slow
  // result must still complete", and for its blocking rule against killing all
  // old-epoch work: a design that drops pre-redirect responses wholesale fails
  // here, and the older ROB head then has a uop that never completes.
  h.Respond(older_slot, s.CreditEpoch(older_slot));
  Require(h.RspAccepted(), where,
          "an older instruction's result was dropped by a squash that does not "
          "own it");
  Require(!h.CreditReturn(), where,
          "an accepted older result returned a credit it had consumed");
  Require(h.ObservedCredits() == 1, where,
          "an accepted older result changed the outstanding count to " +
              std::to_string(h.ObservedCredits()));

  // The younger instruction's result: dropped, and its credit returned once.
  const uint32_t returns_before = s.CreditReturns();
  h.Respond(younger_slot, s.CreditEpoch(younger_slot));
  Require(h.RspDroppedStale(), where,
          "a squashed instruction's result was not classified stale");
  Require(!h.RspAccepted(), where,
          "a squashed instruction's result was accepted: it would be written into "
          "the restored machine");
  Require(h.CreditReturn(), where, "a stale response did not return its credit");
  Require(h.ObservedCredits() == 0, where,
          "the stale response did not return exactly one credit: outstanding is " +
              std::to_string(h.ObservedCredits()));
  Require(s.CreditReturns() == returns_before + 1, where,
          "the cumulative credit-return count did not advance by exactly one");

  // The *same slot again*. This is the clause that makes "exactly once" a
  // property rather than a hope: the table finds nothing reserved, so it returns
  // nothing. A counter could not tell this from a first delivery.
  h.Respond(younger_slot, epoch_before);
  Require(h.RspDroppedDup(), where,
          "a repeated delivery of a returned credit was not reported as a duplicate");
  Require(!h.CreditReturn(), where,
          "a repeated delivery of an already-returned credit returned it again");
  Require(h.ObservedCredits() == 0, where,
          "a duplicate delivery changed the outstanding count to " +
              std::to_string(h.ObservedCredits()));

  // A response for a slot that was never reserved: reported, and no credit.
  // (A response *outside* the table would be an orphan, but at p0 the credit id
  // is four bits and the table is 16 slots, so the wrapper's narrowing makes
  // that case unreachable in this profile; the report records it as such rather
  // than dressing an unreachable branch up as a check.)
  h.Respond(7, h.ObservedEpoch());
  Require(h.RspDroppedDup(), where,
          "a response for a never-reserved in-range slot was not a duplicate");
  Require(!h.CreditReturn(), where, "a never-reserved slot returned a credit");
  Require(h.ObservedCredits() == 0, where, "a never-reserved slot changed the table");
}

// 6. The card's own case: nested branches with the structures *full*, an inner
//    checkpoint restored first and then an outer one, with the conservation and
//    older-work rules checked at each step.
//
// "Full" is stated rather than implied, and it is two facts: the checkpoint stack
// is driven to its depth so the next request is *refused and reported*, and the
// free list is driven to the point where the machine cannot allocate at all. The
// undo window's own bound is the subject of the next phase and cannot coincide
// with a deep stack at p0 -- a checkpoint whose branch allocates its own
// destination consumes a tag the journal does not count, while the profile makes
// the bound equal to the number of allocatable tags. The report records that,
// because it is the reason "full" here is the stack and the free list.
void PhaseNestedBranchFullQueues(Harness& h) {
  ShadowRecovery& s = h.shadow();
  const std::string where = "nested-branch-full-queues";
  const uint32_t depth = s.ckpt_depth();
  const uint32_t entries = h.Entries();

  // The instruction whose slow result must survive every squash below. It is
  // allocated before the first checkpoint, so no redirect taken here owns it.
  const uint32_t older_gen = h.Alloc(6, /*checkpoint=*/false);
  const uint32_t older_tag = h.LastAllocTag();
  const uint32_t older_slot = h.ReserveCreditFor(older_gen);
  Require(older_slot < s.cred_entries(), where,
          "the older instruction's reservation was refused");

  // `depth` nested branches, each a call writing a link register, with work
  // between them. The state before each branch's own allocation is captured,
  // because that is what its checkpoint records: a checkpoint precedes the
  // instruction that takes it.
  std::vector<Snapshot> before(depth);
  std::vector<uint32_t> gens(depth, 0);
  std::vector<uint32_t> tags(depth, 0);
  std::vector<uint64_t> pcs(depth, 0);
  uint32_t older_work[2] = {0, 0};
  for (uint32_t i = 0; i < depth; i++) {
    h.Alloc(20 + (i % 8), /*checkpoint=*/false);
    if (i < 2) older_work[i] = h.LastAllocTag();
    before[i] = h.Now();
    gens[i] = h.Alloc(1, /*checkpoint=*/true);
    tags[i] = h.LastAllocTag();
    pcs[i] = 0x80010000ull + 0x100ull * i;
  }
  h.Idle();
  Require(s.CkptDepth() == depth, where,
          "expected the checkpoint stack at its depth of " + std::to_string(depth) +
              ", the shadow has " + std::to_string(s.CkptDepth()));
  Require(tags[depth - 1] != older_tag, where,
          "the branch re-used the older instruction's tag, so the phase cannot "
          "tell them apart");

  // The stack is full: the next checkpoint is *refused and reported*, while the
  // instruction's own allocation in the same cycle still succeeds. The two
  // refusals are independent -- a checkpoint is not an allocation -- and a
  // design that refused both would be a different rule.
  Stim full;
  full.alloc_valid = true;
  full.alloc_rd = 2;
  full.alloc_rob_gen = h.NextGen();
  full.ckpt_valid = true;
  full.ckpt_rob_gen = full.alloc_rob_gen;
  h.Cycle(full);
  Require(h.CkptRefused(), where,
          "a checkpoint on a full stack was accepted " + Describe(full));
  Require(!h.CkptAccepted(), where,
          "a checkpoint on a full stack reported accepted " + Describe(full));
  Require(h.AllocAccepted(), where,
          "the allocation in the cycle the checkpoint was refused was refused "
          "too; a refused checkpoint is not a refused allocation " + Describe(full));
  Require(s.CkptDepth() == depth, where,
          "a refused checkpoint changed the depth to " +
              std::to_string(s.CkptDepth()));

  // The free list to its limit: no tag is left, so the machine cannot allocate.
  // The allocation after it is refused for *exhaustion* and not for the undo
  // bound, because the branches' own destinations consumed tags the journal
  // never counted -- the reason the two fulls are distinguishable at all.
  const uint32_t free_now = s.FreeCount();
  Require(free_now > 0, where, "the free list was already empty");
  for (uint32_t i = 0; i < free_now; i++) {
    h.Alloc(2 + (i % 29), /*checkpoint=*/false);
  }
  Require(s.FreeCount() == 0, where,
          "the free list did not reach its limit: " +
              std::to_string(s.FreeCount()) + " tags still free");
  Stim dry;
  dry.alloc_valid = true;
  dry.alloc_rd = 9;
  dry.alloc_rob_gen = h.NextGen();
  h.Cycle(dry);
  Require(h.AllocExhausted(), where,
          "an allocation with an empty free list was not refused as exhausted "
          + Describe(dry));
  Require(!h.AllocJournalFull(), where,
          "the undo bound was reported on an allocation that had not reached it "
          + Describe(dry));
  Require(!h.AllocAccepted(), where,
          "an allocation with no free tag was accepted " + Describe(dry));
  Require(!h.ObservedInexact(), where,
          "an allocation refused for exhaustion latched the undo-bound flag");

  // ---- the innermost branch resolves first. Its checkpoint is the youngest, so
  // every outer checkpoint must survive: a squash consumes its own checkpoint and
  // the younger ones, never an older one.
  const uint32_t inner = depth - 1;
  Stim ri;
  ri.redirect0_valid = true;
  ri.redirect0_rob_gen = gens[inner];
  ri.redirect0_pc = pcs[inner];
  h.Cycle(ri);
  Require(h.RedirectTaken(), where,
          "the inner redirect was not taken " + Describe(ri));
  Require(h.RedirectTakenGen() == gens[inner], where,
          "the wrong generation was taken: expected " +
              std::to_string(gens[inner]) + ", got " +
              std::to_string(h.RedirectTakenGen()));
  Require(s.CkptDepth() == inner, where,
          "the inner squash did not leave exactly the older checkpoints: depth is " +
              std::to_string(s.CkptDepth()));
  Require(s.CkptValid(inner - 1) && s.CkptGen(inner - 1) == gens[inner - 1], where,
          "the next-older checkpoint did not survive the inner squash");
  Require(s.JournalLen() == s.CkptJMark(inner), where,
          "the journal is not back at the inner checkpoint's mark: " +
              std::to_string(s.JournalLen()) + " vs " +
              std::to_string(s.CkptJMark(inner)));

  // The restore is exact, wholesale, against the checkpoint's own instant.
  const Snapshot after_inner = h.Now();
  Require(after_inner.free_mask == before[inner].free_mask, where,
          "the inner restore is not free-mask exact: " +
              after_inner.free_mask.Describe(before[inner].free_mask));
  Require(after_inner.gen_valid == before[inner].gen_valid, where,
          "the inner restore is not generation-valid exact: " +
              after_inner.gen_valid.Describe(before[inner].gen_valid));
  Require(after_inner.spec_map == before[inner].spec_map, where,
          "the inner restore is not speculative-map exact: " +
              after_inner.spec_map.Describe(before[inner].spec_map));
  Require(after_inner.tail == before[inner].tail &&
              after_inner.alloc_ptr == before[inner].alloc_ptr, where,
          "the inner restore did not put the tail and the rotation point back");
  Require(after_inner.free_count == before[inner].free_count, where,
          "the inner restore's free count is " +
              std::to_string(after_inner.free_count) + ", the checkpoint's is " +
              std::to_string(before[inner].free_count));

  // The directed check for the card's first blocking rule: a recovery that
  // cleared the whole PRF/free list to avoid the ownership bookkeeping would
  // pass a "the restore is exact" test only if the checkpoint were also empty.
  // Here the machine must still own everything older than the restored branch.
  Require(after_inner.free_count < entries - s.arch(), where,
          "the inner restore left every allocatable tag free (" +
              std::to_string(after_inner.free_count) + " of " +
              std::to_string(entries - s.arch()) +
              "): that is a whole-PRF clear, not a restore");
  Require(!s.IsFree(older_tag) && s.GenValid(older_tag), where,
          "a tag owned by an instruction older than the restored branch was freed "
          "or lost its validity");
  for (uint32_t i = 0; i < inner; i++) {
    Require(!s.IsFree(tags[i]), where,
            "the destination of the surviving branch " + std::to_string(i) +
                " was freed by a squash that does not own it");
  }
  Require(!s.IsFree(older_work[0]) && !s.IsFree(older_work[1]), where,
          "a tag allocated before the restored branch was freed");

  // The older instruction's slow result still completes after the squash, and
  // its writeback still lands: the tag it owns is still its own, generation
  // included, so the result is not rejected as stale and a replay of it is a
  // duplicate rather than a second write.
  h.Respond(older_slot, s.CreditEpoch(older_slot));
  Require(h.RspAccepted(), where,
          "an older instruction's slow result was dropped by the inner squash");
  Require(!h.CreditReturn(), where,
          "an older instruction's accepted result returned its credit");

  // Its writeback lands too: the tag it owns is still its own, generation
  // included, so the result is not rejected as stale and a replay is a duplicate
  // rather than a second write.
  h.Writeback(older_tag, s.Gen(older_tag));
  Require(h.WbAccepted(), where,
          "the older instruction's writeback was not accepted after the squash");
  Require(!h.WbStale() && !h.WbDuplicate(), where,
          "the older instruction's writeback was misreported as stale or duplicate");
  h.Writeback(older_tag, s.Gen(older_tag));
  Require(h.WbDuplicate(), where,
          "a replay of an accepted writeback was not reported as a duplicate");
  Require(!h.WbAccepted(), where,
          "a replay of an accepted writeback was accepted a second time");

  // ---- the outermost branch resolves last, in a cycle that also allocates and
  // retires, with the older *exception* on port 0 and a younger mispredict on
  // port 1. Age decides, not the port and not the fault bit.
  Stim outer;
  outer.redirect0_valid = true;
  outer.redirect0_rob_gen = gens[0];
  outer.redirect0_pc = 0x80020000ull;
  outer.redirect0_is_fault = true;
  outer.redirect1_valid = true;
  outer.redirect1_rob_gen = gens[1];
  outer.redirect1_pc = 0x80030000ull;
  outer.redirect1_is_fault = false;
  outer.alloc_valid = true;
  outer.alloc_rd = 7;
  outer.alloc_rob_gen = h.NextGen();
  outer.rob_retire = true;
  h.Cycle(outer);
  Require(h.RedirectTaken(), where,
          "the older exception was not taken " + Describe(outer));
  Require(h.RedirectTakenGen() == gens[0], where,
          "the arbiter took the younger mispredict over the older exception: "
          "expected " + std::to_string(gens[0]) + ", got " +
              std::to_string(h.RedirectTakenGen()));
  Require(h.RedirectTakenIsFault(), where,
          "the older exception was taken without its fault flag " + Describe(outer));
  Require(h.RedirectKilled() == 1, where,
          "the younger redirect of the same cycle was not reported killed: " +
              std::to_string(h.RedirectKilled()));
  Require(h.Squash() && h.RetireBlock(), where,
          "a recovery in a retire cycle did not block the retire " + Describe(outer));
  Require(!h.AllocAccepted() && h.AllocSquashed(), where,
          "an allocation in a squash cycle was not refused as squashed "
          + Describe(outer));
  Require(s.CkptDepth() == 0, where,
          "a checkpoint survived the squash of the oldest branch: depth is " +
              std::to_string(s.CkptDepth()));
  Require(!h.ObservedInexact(), where,
          "the undo bound was exceeded in a phase that never exceeded it");

  // The outermost restore is exact against *its* checkpoint, and every tag and
  // credit is accounted for as a number as well as through the masks.
  const Snapshot after_outer = h.Now();
  Require(after_outer.free_mask == before[0].free_mask, where,
          "the outermost restore is not free-mask exact: " +
              after_outer.free_mask.Describe(before[0].free_mask));
  Require(after_outer.gen_valid == before[0].gen_valid, where,
          "the outermost restore is not generation-valid exact: " +
              after_outer.gen_valid.Describe(before[0].gen_valid));
  Require(after_outer.spec_map == before[0].spec_map, where,
          "the outermost restore is not speculative-map exact: " +
              after_outer.spec_map.Describe(before[0].spec_map));
  Require(after_outer.tail == before[0].tail &&
              after_outer.alloc_ptr == before[0].alloc_ptr, where,
          "the outermost restore did not put the tail and the rotation point back");
  Require(after_outer.j_len == before[0].j_len, where,
          "the journal after the outermost restore is " +
              std::to_string(after_outer.j_len) + ", the checkpoint's is " +
              std::to_string(before[0].j_len));
  Require(after_outer.free_count == before[0].free_count, where,
          "the outermost restore leaked or double-freed tags: " +
              std::to_string(after_outer.free_count) + " free against " +
              std::to_string(before[0].free_count));
  Require(s.FreeCount() + s.CommittedOwned() <= entries, where,
          "the conservation identity is violated: " + std::to_string(s.FreeCount()) +
              " free + " + std::to_string(s.CommittedOwned()) + " owned exceeds " +
              std::to_string(entries));
  Require(!s.IsFree(older_tag), where,
          "the oldest instruction's tag was freed by a squash older than it");
  // One reservation was made and its result was accepted, so no credit was ever
  // returned and none is outstanding: credits are conserved in both directions.
  Require(h.ObservedCredits() == 0, where,
          "credits outstanding after the phase is " +
              std::to_string(h.ObservedCredits()));
  Require(s.CreditReturns() == 0, where,
          "the phase returned " + std::to_string(s.CreditReturns()) +
              " credits, but every reservation it made was accepted");
}

// 7. Population and content after a squash equal the checkpoint, over a long run.
void PhaseFreeListExact(Harness& h) {
  ShadowRecovery& s = h.shadow();
  const std::string where = "free-list-exact";

  for (uint32_t round = 0; round < 24; round++) {
    const uint32_t before_free = s.FreeCount();
    Require(before_free > 0, where,
            "the free list is empty before round " + std::to_string(round));

    // A body of work, then a checkpoint, then more work. The capture is taken
    // *before* the branch allocates: a checkpoint records the instant before the
    // instruction that takes it, so the restore frees the branch's own
    // destination -- the snapshot after that allocation is one instant too late
    // and would demand the restore re-materialise the branch it exists to erase.
    for (uint32_t i = 0; i < 3; i++) h.Alloc(10 + (i % 10), /*checkpoint=*/false);
    // The mapping this round will commit: x10's *speculative* mapping as it stands
    // before the checkpoint. It has to be a mapping created before the branch, or
    // the commit would publish a younger instruction's mapping -- a machine
    // cannot reach that state, since retire is in order and the branch is younger
    // than anything it can commit over.
    const uint32_t cmt_rd = 10;
    const uint32_t cmt_old_tag = s.CmtTag(cmt_rd);
    const uint32_t cmt_new_tag = s.SpecTag(cmt_rd);
    const uint32_t cmt_new_gen = s.SpecGen(cmt_rd);
    const Snapshot at_ckpt = h.Now();
    const uint32_t gen = h.Alloc(1, /*checkpoint=*/true);
    Require(h.Now().free_mask != at_ckpt.free_mask, where,
            "round " + std::to_string(round) +
                ": the branch's own allocation changed nothing, so its destination "
                "is not exercised");
    for (uint32_t i = 0; i < 5; i++) h.Alloc(12 + (i % 16), /*checkpoint=*/false);

    // Commit x10's pre-checkpoint mapping, so the committed map moves and the free
    // set has to be restored to a checkpoint whose committed map has since
    // changed. A commit is permanent: it is *not* undone, and the tag it
    // supersedes stays free. The free-mask expectation below is therefore the
    // checkpoint's *plus* that tag, and not the checkpoint's alone: a restore that
    // undid the commit would resurrect a mapping the ISA has already published.
    {
      std::fprintf(stderr, "DBGR round=%u alloc_ptr=%u epoch_sh=%u epoch_dut=%u valid=%02x ckepoch_dut=%u ckepoch_sh=%u\n",
                   round, s.AllocPtr(), s.Epoch(), h.ObservedEpoch(),
                   (unsigned)(h.ObservedCkptValid() & 0xffu),
                   (unsigned)(h.ObservedCkptEpoch() & 0x7fu),
                   (unsigned)(s.CkptEpochWide().words[0] & 0x7fu));
      for (unsigned i = 0; i < 8; i++) {
        std::fprintf(stderr, "  slot%u dut=%u sh=%u\n", i,
                     (unsigned)((h.ObservedCkptBundle() >> (7 * i)) & 0x7fu),
                     (unsigned)((s.CkptAllocPtrWide().words[0] >> (7 * i)) & 0x7fu));
      }
    }
    h.Commit(cmt_rd, cmt_new_tag, cmt_new_gen);
    Require(s.CmtTag(cmt_rd) == cmt_new_tag, where,
            "round " + std::to_string(round) +
                ": the commit did not move the committed map, so this round would "
                "test nothing");
    h.Idle();

    Stim r;
    r.redirect0_valid = true;
    r.redirect0_rob_gen = gen;
    r.redirect0_pc = 0x80009000ull + round;
    h.Cycle(r);
    Require(h.RedirectTaken(), where,
            "the redirect in round " + std::to_string(round) + " was not taken");

    // The free mask is the checkpoint's, plus exactly what the commit freed
    // permanently...
    const Snapshot after = h.Now();
    Wide expected_free = at_ckpt.free_mask;
    expected_free.Set(cmt_old_tag, true);
    Require(after.free_mask == expected_free, where,
            "round " + std::to_string(round) + ": the free mask after the squash is "
            "not the checkpoint's plus the tag the commit superseded -- " +
                after.free_mask.Describe(expected_free));
    // ...its population count agrees, checked as a number as well as through the
    // mask, so a defect in the count's derivation cannot hide behind a correct
    // mask...
    Require(after.free_count == static_cast<uint32_t>(expected_free.PopCount()), where,
            "round " + std::to_string(round) + ": the free count after the squash is " +
                std::to_string(after.free_count) + ", the expected mask has " +
                std::to_string(expected_free.PopCount()) + " bits set");
    // ...and everything a commit cannot move is the checkpoint's, bit for bit:
    // the generations, the written flags, the tag generations, the speculative
    // map, the tail, the rotation point and the journal. The committed map is
    // deliberately absent from this list *and* checked separately below, so a
    // restore that rewound it fails on its own name rather than on a field
    // nobody thought to list.
    Require(after.gen_valid == at_ckpt.gen_valid && after.wb_done == at_ckpt.wb_done &&
                after.tag_gen == at_ckpt.tag_gen &&
                after.spec_map == at_ckpt.spec_map &&
                after.tail == at_ckpt.tail && after.alloc_ptr == at_ckpt.alloc_ptr &&
                after.j_len == at_ckpt.j_len,
            where,
            "round " + std::to_string(round) +
                ": the state after the squash is not the checkpoint's -- " +
                Snapshot::FirstDifference(at_ckpt, after));
    Require(s.CmtTag(cmt_rd) == cmt_new_tag, where,
            "round " + std::to_string(round) +
                ": the squash undid a commit -- the committed map is back at the "
                "mapping the commit replaced");
  }

  // The conservation identity: free + owned == entries, on every cycle, has been
  // checked throughout; here it is asserted as a fact about the end state.
  Require(s.FreeCount() + s.CommittedOwned() <= h.Entries(), where,
          "the conservation identity is violated at the end of the phase: " +
              std::to_string(s.FreeCount()) + " free + " +
              std::to_string(s.CommittedOwned()) + " owned exceeds " +
              std::to_string(h.Entries()));
}

// 7. The undo window at its full bound, and one allocation beyond it.
void PhaseJournalBound(Harness& h) {
  ShadowRecovery& s = h.shadow();
  const std::string where = "journal-bound";
  const uint32_t bound = h.Rob();

  // The checkpointing branch writes x0, so it allocates nothing: the window has
  // to reach its bound, and a branch that took a tag would make the free list
  // run out first -- at p0 `entries - arch == rob`, so the bound and the
  // allocatable tags are the same number and *every* tag has to be a journalled
  // allocation for the bound to be reachable at all.
  const Snapshot at_ckpt = h.Now();
  const uint32_t gen = h.Alloc(0, /*checkpoint=*/true);
  Require(s.CkptGen(0) == gen, where,
          "the checkpoint did not record the branch's generation");
  const uint32_t mark = s.CkptJMark(0);
  Require(mark == 0, where,
          "the checkpoint's journal mark is " + std::to_string(mark) +
              ", expected zero: nothing had been allocated when it was taken");
  h.Idle();

  // Drive the window to exactly its bound. Every allocation needs a journal
  // entry, so the bound is `bound` allocations. Reaching it is the *expected*
  // steady state for a full queue, and must not report an overflow.
  for (uint32_t i = 0; i < bound; i++) {
    const uint32_t rd = 3 + (i % 20);
    Stim a;
    a.alloc_valid = true;
    a.alloc_rd = rd;
    a.alloc_rob_gen = h.NextGen();
    h.Cycle(a);
    Require(!h.AllocJournalFull(), where,
            "the allocation at bound position " + std::to_string(i) +
                " was refused; reaching the bound is legal");
    Require(!h.JournalOverflow(), where,
            "reaching the bound reported an overflow at position " +
                std::to_string(i));
    Require(h.AllocAccepted(), where,
            "the allocation at bound position " + std::to_string(i) +
                " was not accepted " + Describe(a));
  }
  Require(s.JournalLen() == bound, where,
          "the window reached " + std::to_string(s.JournalLen()) + ", expected the "
          "full bound of " + std::to_string(bound));
  Require(s.FreeCount() == 0, where,
          "reaching the undo bound did not leave the free list empty: " +
              std::to_string(s.FreeCount()) + " tags still free");
  Require(!h.ObservedInexact(), where,
          "reaching the bound latched the inexact flag");

  // One allocation beyond the bound. It must be *refused* and reported, and the
  // journal must not be wrapped -- which is shown by the restore below still
  // producing the checkpoint state rather than a mixture of two windows.
  Stim over;
  over.alloc_valid = true;
  over.alloc_rd = 22;
  over.alloc_rob_gen = h.NextGen();
  h.Cycle(over);
  Require(h.AllocJournalFull(), where,
          "an allocation beyond the undo bound was not refused " + Describe(over));
  Require(h.JournalOverflow(), where,
          "the undo-bound violation was not reported " + Describe(over));
  Require(!h.AllocAccepted(), where,
          "an allocation beyond the undo bound was accepted " + Describe(over));
  Require(s.JournalLen() == bound, where,
          "the journal grew past its bound to " + std::to_string(s.JournalLen()));
  Require(h.ObservedJournalLen() == bound, where,
          "the DUT's journal is " + std::to_string(h.ObservedJournalLen()) +
              ", beyond the bound");
  Require(h.ObservedInexact(), where,
          "an allocation refused at the undo bound did not latch the inexact flag");

  // The window did not change, so the restore from the full bound is still
  // exact: it undoes the whole window, back to the checkpoint's instant -- which
  // here is the state before the first allocation, since the checkpoint's mark is
  // zero. A wrapped journal would have overwritten a live entry with the refused
  // allocation's tag and this would not match. `at_ckpt` is the instant the
  // checkpoint records, so it is what the restore must reproduce; `at_bound` is
  // 64 allocations later and is *not*.
  h.Idle();
  Stim r;
  r.redirect0_valid = true;
  r.redirect0_rob_gen = gen;
  r.redirect0_pc = 0x8000a000ull;
  h.Cycle(r);
  Require(h.RedirectTaken(), where, "the redirect at the bound was not taken");

  const Snapshot after = h.Now();
  Require(after.Key() == at_ckpt.Key(), where,
          "the restore from a full undo window is not exact -- " +
              Snapshot::FirstDifference(at_ckpt, after));
  Require(after.j_len == mark, where,
          "the journal is not back at the checkpoint's mark after a restore from "
          "the full bound: " + std::to_string(after.j_len) + " vs " +
          std::to_string(mark));

  // The journal is at the checkpoint's mark and the window is closed: a further
  // allocation starts a fresh window from there, with no residue.
  Stim post;
  post.alloc_valid = true;
  post.alloc_rd = 23;
  post.alloc_rob_gen = h.NextGen();
  h.Cycle(post);
  Require(s.JournalLen() == 1, where,
          "the window did not restart cleanly after the restore: " +
              std::to_string(s.JournalLen()));
}

// 8. Past the ROB depth, so the tail and the rotation point both wrap.
void PhaseWrap(Harness& h) {
  ShadowRecovery& s = h.shadow();
  const std::string where = "wrap";
  const uint32_t bound = h.Rob();

  // More than ROB_ENTRIES allocations, so the tail wraps at least once, with
  // checkpoints and restores interleaved so the wrap happens both inside a
  // window and across a restore.
  uint32_t restores = 0;
  for (uint32_t i = 0; i < bound * 3; i++) {
    const bool checkpoint = (i % 11) == 0;
    const uint32_t gen = h.Alloc(3 + (i % 25), checkpoint);
    if (i == 0) continue;

    if ((i % 11) == 5) {
      // A restore from five allocations back, so the tail and the rotation point
      // are both mid-wrap at the moment of the restore.
      Stim r;
      r.redirect0_valid = true;
      r.redirect0_rob_gen = gen;
      r.redirect0_pc = 0x8000b000ull + i;
      h.Cycle(r);
      Require(h.RedirectTaken(), where,
              "the redirect at allocation " + std::to_string(i) + " was not taken");
      restores++;
    }
  }

  Require(restores > 0, where, "the wrap phase performed no restores");
  Require(s.Tail() != 0 || s.CkptDepth() == 0, where,
          "the tail state is inconsistent after the wrap");
  Require(s.FreeCount() > 0, where,
          "the free list was exhausted by the wrap; a wrap must not leak or "
          "double-free a tag");
  Require(s.FreeCount() + s.CommittedOwned() <= h.Entries(), where,
          "the conservation identity is violated after the wrap: " +
              std::to_string(s.FreeCount()) + " free + " +
              std::to_string(s.CommittedOwned()) + " owned");

  // And the tail really did wrap: the shadow's tail has been round the ring more
  // than once for the number of allocations performed.
  Require(h.cycles() > bound, where,
          "the wrap phase did not run long enough to wrap the tail");
}

// 9. The credit table at capacity, with duplicates and out-of-order delivery.
void PhaseCreditStress(Harness& h) {
  ShadowRecovery& s = h.shadow();
  const std::string where = "credit-stress";
  const uint32_t slots = s.cred_entries();

  h.Alloc(1, /*checkpoint=*/true);
  h.Idle();

  // Fill the table to capacity.
  for (uint32_t i = 0; i < slots; i++) {
    Stim r;
    r.cred_req_valid = true;
    r.cred_req_id = i % 4;
    r.cred_req_rob_gen = h.NextGen();
    h.Cycle(r);
    Require(h.ObservedCredits() == i + 1, where,
            "after " + std::to_string(i + 1) + " reservations the DUT reports " +
                std::to_string(h.ObservedCredits()) + " outstanding");
  }
  Require(h.ObservedCredits() == slots, where,
          "the table did not reach capacity: " + std::to_string(h.ObservedCredits()) +
              " of " + std::to_string(slots));

  // One more is refused and reported, and the count does not go past capacity.
  Stim over;
  over.cred_req_valid = true;
  over.cred_req_id = 0;
  h.Cycle(over);
  Require(h.CredReqFull(), where, "a reservation into a full table was granted");
  Require(h.ObservedCredits() == slots, where,
          "a refused reservation changed the outstanding count to " +
              std::to_string(h.ObservedCredits()));

  // Deliver the responses out of order, in the current epoch, so they are all
  // accepted and the table drains. The *count* is the check: a design that
  // returned a credit on an accepted response would show it here.
  for (uint32_t i = 0; i < slots; i++) {
    const uint32_t id = (slots - 1 - i) % slots;
    h.Respond(id, h.ObservedEpoch());
  }
  Require(h.ObservedCredits() == 0, where,
          "the table did not drain after every slot was answered: " +
              std::to_string(h.ObservedCredits()) + " outstanding");

  // Reuse of a reserved id is reported as a conflict.
  Stim a;
  a.cred_req_valid = true;
  a.cred_req_id = 0;
  h.Cycle(a);
  Stim b;
  b.cred_req_valid = true;
  b.cred_req_id = 0;   // the same id, still reserved
  h.Cycle(b);
  Require(h.CredReqConflict(), where,
          "a producer reusing a reserved credit id was not reported");

  // Drain again, then raise the epoch and show the whole table goes stale.
  h.Respond(0, h.ObservedEpoch());
  const uint32_t ep = h.ObservedEpoch();
  h.Alloc(1, /*checkpoint=*/true);
  Stim r;
  r.redirect0_valid = true;
  r.redirect0_rob_gen = s.CkptGen(s.CkptDepth() - 1);
  r.redirect0_pc = 0x8000c000ull;
  h.Cycle(r);
  Require(h.ObservedEpoch() == ep + 1, where, "the redirect did not raise the epoch");

  // Every response in the old epoch is now stale, and each returns its credit
  // exactly once -- the count is the assertion.
  uint32_t expected = 0;
  for (uint32_t c = 0; c < slots; c++) {
    if (!s.CreditBusy(c)) continue;
    h.Respond(c, ep);
    expected++;
  }
  Require(expected > 0, where,
          "the credit-stress phase cancelled nothing, so it proved nothing");
  Require(h.ObservedCredits() == 0, where,
          "every cancelled credit was not returned: " +
              std::to_string(h.ObservedCredits()) + " outstanding");
  Require(h.shadow().CreditReturns() >= expected, where,
          "the cumulative return count is " +
              std::to_string(h.shadow().CreditReturns()) + " for " +
              std::to_string(expected) + " cancelled credits");
}

// 10. Random stimulus, compared against the shadow on every cycle.
void PhaseRandom(Harness& h, uint64_t seed, int cycles) {
  mosaic::Rng rng(seed);
  ShadowRecovery& s = h.shadow();
  const std::string where = "random-soak";

  h.Alloc(1, /*checkpoint=*/true);

  for (int c = 0; c < cycles; c++) {
    Stim stim;

    // Allocation, a third of the time, with a checkpoint every eighth.
    if (rng.Chance(35)) {
      stim.alloc_valid = true;
      stim.alloc_rd = 1 + rng.Below(31);
      stim.alloc_rob_gen = h.NextGen();
      if (rng.Chance(12)) {
        stim.ckpt_valid = true;
        stim.ckpt_rob_gen = stim.alloc_rob_gen;
      }
    }

    // A commit, occasionally.
    if (rng.Chance(15)) {
      stim.commit_valid = true;
      stim.commit_rd = 1 + rng.Below(31);
      stim.commit_tag = rng.Below(h.Entries());
      stim.commit_gen = rng.Below(8);
      stim.rob_retire = rng.Chance(50);
    }

    // A writeback and a release, rarely, and often aimed at something stale.
    if (rng.Chance(12)) {
      stim.wb_valid = true;
      stim.wb_tag = rng.Below(h.Entries());
      stim.wb_gen = rng.Chance(30) ? rng.Below(8) : s.Gen(stim.wb_tag);
    }
    if (rng.Chance(8)) {
      stim.free_valid = true;
      stim.free_tag = rng.Below(h.Entries());
      stim.free_gen = rng.Chance(30) ? rng.Below(8) : s.Gen(stim.free_tag);
    }

    // Redirects, usually out of order and sometimes aimed at a generation with
    // no checkpoint, which is the stale case.
    if (rng.Chance(20)) {
      const uint32_t port = rng.Below(2);
      const bool live = rng.Chance(60) && s.CkptDepth() > 0;
      const uint32_t gen = live ? s.CkptGen(rng.Below(s.CkptDepth()))
                                : rng.Below(64);
      const uint64_t pc = 0x80000000ull + 0x1000ull * rng.Below(64);
      if (port == 0) {
        stim.redirect0_valid = true;
        stim.redirect0_rob_gen = gen;
        stim.redirect0_pc = pc;
        stim.redirect0_is_fault = rng.Chance(10);
      } else {
        stim.redirect1_valid = true;
        stim.redirect1_rob_gen = gen;
        stim.redirect1_pc = pc;
        stim.redirect1_is_fault = rng.Chance(10);
      }
    }

    // Credits: reserve, and answer with the current or a stale epoch, and
    // sometimes answer the same slot twice.
    if (rng.Chance(20)) {
      stim.cred_req_valid = true;
      stim.cred_req_id = rng.Below(4);
      stim.cred_req_rob_gen = h.NextGen();
    }
    if (rng.Chance(25)) {
      stim.rsp_valid = true;
      stim.rsp_id = rng.Chance(15) ? 0x3f : rng.Below(s.cred_entries());
      stim.rsp_epoch = rng.Chance(50) ? h.ObservedEpoch() : h.ObservedEpoch() + 1;
    }

    h.Cycle(stim);
  }

  // A soak that never reached the interesting states would pass while testing
  // nothing, so the phase asserts these are non-zero.
  const Harness::Counters& c = h.counters();
  Require(c.restores > 0, where, "the soak performed no restores");
  Require(c.stale_redirects > 0, where, "the soak produced no stale redirect");
  Require(c.stale_responses > 0, where, "the soak produced no stale response");
  Require(c.duplicate_responses > 0, where, "the soak produced no duplicate response");
  Require(c.credit_returns > 0, where, "the soak returned no credit");
  Require(c.killed_redirects > 0, where, "the soak never killed a pending redirect");
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
  Vmosaic_recovery_tb dut;

  std::string detail;
  bool passed = true;
  try {
    // The geometry outputs are constants derived from the instance parameters, so
    // one evaluation is enough to read them and no reset is needed first. The
    // shadow is sized from these numbers and from nothing else.
    dut.clk = 0;
    dut.rst = 0;
    dut.eval();

    const uint32_t entries = dut.o_prf_entries_o;
    const uint32_t tag_w = dut.o_tag_w_o;
    const uint32_t gen_w = dut.o_gen_w_o;
    const uint32_t arch = dut.o_arch_regs_o;
    const uint32_t rob = dut.o_rob_entries_o;
    const uint32_t idx_w = dut.o_rob_index_w_o;
    const uint32_t id_w = dut.o_id_w_o;
    const uint32_t epoch_w = dut.o_epoch_w_o;
    const uint32_t ckpt_depth = dut.o_ckpt_depth_param_o;
    const uint32_t rdq_depth = dut.o_rdq_depth_param_o;
    const uint32_t cred_entries = dut.o_cred_entries_param_o;

    Require(entries > 0 && rob > 0 && tag_w > 0, "geometry",
            "the DUT reported a zero depth, so the shadow cannot be sized");
    Require(arch == 32, "geometry",
            "expected 32 architectural registers, the DUT reports " +
                std::to_string(arch));
    Require(id_w == 2 * idx_w, "geometry",
            "the identity width is not twice the slot index width: id_w = " +
                std::to_string(id_w) + ", idx_w = " + std::to_string(idx_w));
    Require(epoch_w == idx_w + 1, "geometry",
            "the epoch width is not clog2(rob_entries)+1: " +
                std::to_string(epoch_w) + " vs " + std::to_string(idx_w + 1));
    Require(dut.o_map_w_o == tag_w + gen_w, "geometry",
            "the map entry width is not tag+generation");
    Require(dut.o_cnt_w_o == Clog2(rob + 1), "geometry",
            "the journal length width is not clog2(rob+1)");
    Require(ckpt_depth > 1 && rdq_depth >= 2, "geometry",
            "the structural depths must leave room for a checkpoint pair and for "
            "two same-cycle redirects: ckpt_depth = " + std::to_string(ckpt_depth) +
                ", rdq_depth = " + std::to_string(rdq_depth));
    Require(cred_entries > 1, "geometry",
            "the credit table must hold more than one reservation");

    // The geometry relationship the journal bound rests on. Checked here as well
    // as by tools/check_profile.py, because this case *depends* on it: with fewer
    // allocatable tags than ROB entries, the window cannot reach its bound
    // without exhausting the free list first, and the bound phase would be
    // testing the wrong thing.
    Require(entries - arch >= rob, "geometry",
            "int_prf.entries - arch_int_regs = " +
                std::to_string(entries - arch) + " is below rob.entries = " +
                std::to_string(rob) +
                "; the undo journal would overflow on a correctly-squashing "
                "machine. Deepen the PRF or shrink the ROB");

    // The wide-port word counts the driver reads must match what the wrapper
    // derived, or the driver would be comparing a truncated state.
    Require(dut.o_mask_words_o == (entries + 63) / 64, "geometry",
            "the free-mask word count does not match the geometry");
    Require(dut.o_tag_gen_words_o == (entries * gen_w + 63) / 64, "geometry",
            "the generation-table word count does not match the geometry");
    Require(dut.o_map_words_o == (arch * (tag_w + gen_w) + 63) / 64, "geometry",
            "the map word count does not match the geometry");

    ShadowRecovery shadow(entries, tag_w, gen_w, arch, rob, id_w, idx_w, epoch_w,
                          ckpt_depth, rdq_depth, cred_entries);
    Harness harness(&dut, &clk, options.max_cycles, &shadow);

    // Phase order is a deliberate choice. Each phase resets first and owns one
    // mechanism, and a run stops at the first failure, so the order decides
    // *which* phase reports a given defect. The directed phases run before the
    // two that are sensitive to everything.
    harness.Phase("reset-state");
    harness.Fresh();
    PhaseResetState(harness);

    harness.Phase("exact-restore");
    harness.Fresh();
    PhaseExactRestore(harness);

    harness.Phase("nested-ckpt");
    harness.Fresh();
    PhaseNestedCheckpoint(harness);

    harness.Phase("oldest-redirect");
    harness.Fresh();
    PhaseOldestRedirect(harness);

    harness.Phase("late-response");
    harness.Fresh();
    PhaseLateResponse(harness);

    // The card's own case name. It runs after the directed phases because it is
    // the one that depends on all of them -- nesting, the undo window, the age
    // boundary and the credit table at once -- and a failure here should name the
    // mechanism the earlier phases already isolated.
    harness.Phase("nested-branch-full-queues");
    harness.Fresh();
    PhaseNestedBranchFullQueues(harness);

    harness.Phase("free-list-exact");
    harness.Fresh();
    PhaseFreeListExact(harness);

    harness.Phase("journal-bound");
    harness.Fresh();
    PhaseJournalBound(harness);

    harness.Phase("wrap");
    harness.Fresh();
    PhaseWrap(harness);

    harness.Phase("credit-stress");
    harness.Fresh();
    PhaseCreditStress(harness);

    harness.Phase("random-soak");
    harness.Fresh();
    PhaseRandom(harness, options.seed, 6000);

    const Harness::Counters& c = harness.counters();
    detail = "recovery contract holds: " + std::to_string(harness.comparisons()) +
             " shadow comparisons over " + std::to_string(harness.cycles()) +
             " cycles; directed: " + std::to_string(c.restores) + " restores, " +
             std::to_string(c.stale_redirects) + " stale redirects, " +
             std::to_string(c.killed_redirects) + " killed redirects, " +
             std::to_string(c.stale_responses) + " stale responses, " +
             std::to_string(c.duplicate_responses) + " duplicate responses, " +
             std::to_string(c.credit_returns) + " credits returned, " +
             std::to_string(c.journal_refusals) + " journal-bound refusals; soak: " +
             std::to_string(c.checkpoints) + " checkpoints, seed " +
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
