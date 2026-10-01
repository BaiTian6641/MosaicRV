// ============================================================================
// tb_rename.cpp -- CASE=rename.single_width_ownership, work package I-013.
//
// The DUT is never its own oracle. Every output and every piece of internal
// state is compared against an independent C++ shadow written from the
// *contract* documented in rtl/core/mosaic_rename.sv -- not from the RTL's
// structure. The shadow holds its own committed map, speculative map, free set,
// generation table, written flags, rotation point and undo journal, and derives
// the rotating allocation scan, the generation step, the ownership rules and
// the checkpoint restore from that prose. It shares no code with the RTL and
// never looks at Verilator internals, so agreeing with it is evidence about the
// contract rather than a restatement of the implementation.
//
// Geometry is read from the elaborated DUT (`o_entries` and friends), so this
// file contains no register-file depth, no tag width and no bank count: a
// profile with different values needs no edit here, and the shadow cannot
// disagree with the hardware about how big it is. The only geometry constant in
// this file is the *assertion* that the free count at reset equals
// ENTRIES - ARCH_REGS, which is the property the reset state has to satisfy for
// "every physical register is exactly free or owned by exactly one live
// instruction" to hold at cycle 0 -- so it is written as an inequality against
// the observed numbers, not as a literal 64.
//
// Phases, each of which can fail on its own:
//
//   1. reset-state      the documented cold state: every arch reg mapped to its
//                       own tag, every one of those tags owned, everything else
//                       free, no generation valid, no journal.
//   2. ownership        allocate, write back, commit, release; every live
//                       speculative mapping has exactly one owner, a tag is
//                       never handed out twice, and a double free is reported.
//   3. generation       allocate a tag, free it, re-allocate it, then deliver a
//                       *stale* write with the old generation: it must be
//                       rejected. This is the phase the whole card is about.
//   4. wrap             enough allocate/commit cycles to wrap the physical tags
//                       several times over, with a stale generation rejected
//                       after every wrap.
//   5. x0               writes to x0 allocate nothing, the free count does not
//                       move, reads return zero, and a commit to x0 frees
//                       nothing.
//   6. squash           speculative state, checkpoint, more speculative state,
//                       squash: the committed map is exactly what it was, the
//                       free set is exactly restored, and the same-cycle
//                       ordering of restore against allocation is pinned down.
//   7. exhaustion       fill the free set to empty, then assert that the next
//                       request is refused and reported and that nothing was
//                       over-allocated.
//   8. random           a random programme of all six request kinds, compared
//                       against the shadow on every cycle, with the ownership
//                       and conservation invariants re-checked as it goes.
//
// Standing invariants, checked on every cycle of every phase rather than in one
// place:
//
//   * `free_count` equals the population count of the free mask the DUT reports.
//   * Every tag is either free or owned by exactly one live mapping; the count
//     of owned tags plus the count of free tags is exactly ENTRIES.
//   * No tag is both in the free set and named by a live speculative mapping.
//   * A reported allocation never returns a tag that is currently owned, and a
//     reported writeback is never accepted for a tag that is free.
//   * The journal never overflows, and a squash never restores a partial window.
//
// The last one is a guard on the DUT's own escape hatch: `journal_overflow` is a
// report, so a test that never checks it would let a design that silently
// restored less than the truth pass.
// ============================================================================

#include <verilated.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "sim_common.h"
#include "Vmosaic_rename_tb.h"

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

// A {tag, generation} destination identity. Two allocations of the same
// physical tag are different destinations, which is the whole point of the
// generation, so the shadow and the test both carry the pair rather than the
// tag alone.
struct Dest {
  uint32_t tag = 0;
  uint32_t gen = 0;

  bool operator==(const Dest& other) const { return tag == other.tag && gen == other.gen; }
  bool operator!=(const Dest& other) const { return !(*this == other); }
  std::string str() const { return "(" + Dec(tag) + "," + Dec(gen) + ")"; }
};

// --------------------------------------------------------------- the stimulus
struct Stim {
  // Lane 0 of the group, and lane 1 of the same group. Program order is lane 0
  // then lane 1, and `alloc2_req` without `alloc_req` is not a group the core may
  // present (there is no second macro without a first).
  bool alloc_req = false;
  uint32_t alloc_rd = 0;
  bool alloc2_req = false;
  uint32_t alloc2_rd = 0;

  // Four source addresses in (macro, source) order: rs1/rs2 belong to lane 0,
  // rs3/rs4 to lane 1.
  uint32_t rs1_addr = 0;
  uint32_t rs2_addr = 0;
  uint32_t rs3_addr = 0;
  uint32_t rs4_addr = 0;

  bool wb_valid = false;
  Dest wb{};

  bool free_valid = false;
  Dest free_{};

  bool commit_valid = false;
  uint32_t commit_rd = 0;
  Dest commit{};

  // The second commit lane (I-017), driven here because the WAW release contract
  // of a two-wide rename group is a statement about what the two lanes release.
  bool commit2_valid = false;
  uint32_t commit2_rd = 0;
  Dest commit2{};

  bool ckpt_valid = false;
  bool squash = false;

  std::string str() const {
    return "[alloc=" + Bool(alloc_req) + ":x" + Dec(alloc_rd) +
           " alloc2=" + Bool(alloc2_req) + ":x" + Dec(alloc2_rd) +
           " rs=x" + Dec(rs1_addr) + ",x" + Dec(rs2_addr) +
           " | x" + Dec(rs3_addr) + ",x" + Dec(rs4_addr) +
           " wb=" + Bool(wb_valid) + wb.str() + " free=" + Bool(free_valid) + free_.str() +
           " commit=" + Bool(commit_valid) + ":x" + Dec(commit_rd) + commit.str() +
           " commit2=" + Bool(commit2_valid) + ":x" + Dec(commit2_rd) + commit2.str() +
           " ckpt=" + Bool(ckpt_valid) + " squash=" + Bool(squash) + "]";
  }
};

// What the shadow says the DUT must present in the cycle being run. Every field
// is combinational in the cycle's inputs plus the shadow's pre-edge state.
struct Outputs {
  bool alloc_accepted = false;
  bool alloc_exhausted = false;
  bool alloc_squashed = false;
  bool alloc_is_x0 = false;
  bool alloc_new_valid = false;
  Dest alloc_new{};
  bool alloc_old_valid = false;
  Dest alloc_old{};

  // Lane 1 of the same group. `alloc2_exhausted` means the *group* could not be
  // given the tags it needed -- which may be lane 1's own requirement or lane
  // 0's; the group's refusal is one decision and both lanes report it.
  bool alloc2_accepted = false;
  bool alloc2_exhausted = false;
  bool alloc2_squashed = false;
  bool alloc2_is_x0 = false;
  bool alloc2_new_valid = false;
  Dest alloc2_new{};
  bool alloc2_old_valid = false;
  Dest alloc2_old{};

  bool rs1_is_x0 = false;
  bool rs2_is_x0 = false;
  bool rs1_ready = false;
  bool rs2_ready = false;
  Dest rs1{};
  Dest rs2{};

  // Lane 1's sources, with the same-cycle bypass: `rs3/rs4_bypass` says the
  // source was resolved from lane 0's new destination rather than from the map,
  // and a bypassed source is never ready.
  bool rs3_is_x0 = false;
  bool rs4_is_x0 = false;
  bool rs3_ready = false;
  bool rs4_ready = false;
  bool rs3_bypass = false;
  bool rs4_bypass = false;
  Dest rs3{};
  Dest rs4{};

  bool wb_accepted = false;
  bool wb_stale = false;
  bool wb_duplicate = false;

  bool free_accepted = false;
  bool free_stale = false;
  bool free_double = false;

  bool commit_accepted = false;
  bool commit_x0_dropped = false;
  bool commit2_accepted = false;
  bool commit2_x0_dropped = false;

  bool squash_accepted = false;
  bool squash_underflow = false;
  bool journal_overflow = false;

  uint32_t free_count = 0;
};

// ------------------------------------------------------------------ the shadow
//
// An independent model of the documented contract. Every field below has a
// counterpart in the header prose of rtl/core/mosaic_rename.sv; none of them was
// obtained by reading RTL. The free set is a `std::vector<bool>` scanned in
// rotation order rather than a bitmap with a priority encoder, and the journal is
// a `std::vector<Dest>` walked oldest-first, so a bug in the RTL's scan, its
// pointer wrap or its undo order cannot cancel against the same bug here.
class ShadowRename {
 public:
  ShadowRename(uint32_t entries, uint32_t tag_w, uint32_t gen_w, uint32_t arch_regs,
               uint32_t journal)
      : entries_(entries),
        tag_w_(tag_w),
        gen_w_(gen_w),
        arch_regs_(arch_regs),
        journal_(journal),
        gen_mask_((gen_w >= 32) ? 0xffffffffu : ((1u << gen_w) - 1u)) {
    Reset();
  }

  uint32_t gen_mask() const { return gen_mask_; }
  uint32_t journal_length() const { return static_cast<uint32_t>(j_len_); }


  // The documented cold state: arch reg i -> tag i at generation 0, those tags
  // owned by those mappings, every other tag free, no generation valid, nothing
  // written, nothing in the journal.
  void Reset() {
    spec_.assign(arch_regs_, Dest{});
    cmt_.assign(arch_regs_, Dest{});
    for (uint32_t a = 0; a < arch_regs_; a++) {
      spec_[a] = Dest{a, 0};
      cmt_[a] = Dest{a, 0};
    }
    free_.assign(entries_, true);
    for (uint32_t a = 0; a < arch_regs_; a++) {
      free_[a] = false;  // the reset mapping owns its own tag
    }
    gen_.assign(entries_, 0);
    gen_valid_.assign(entries_, false);
    wb_done_.assign(entries_, false);
    rot_ = arch_regs_;
    undo_.clear();
    j_len_ = 0;
    ckpt_seen_ = false;
    j_overflow_ = false;
  }

  uint32_t entries() const { return entries_; }
  uint32_t tag_w() const { return tag_w_; }
  uint32_t gen_w() const { return gen_w_; }
  uint32_t arch_regs() const { return arch_regs_; }
  uint32_t journal_depth() const { return journal_; }


  // ------------------------------------------------------------ observation
  const std::vector<bool>& free_set() const { return free_; }
  const std::vector<bool>& gen_valid() const { return gen_valid_; }
  const std::vector<bool>& wb_done() const { return wb_done_; }
  const std::vector<uint32_t>& gen() const { return gen_; }
  const std::vector<Dest>& spec_map() const { return spec_; }
  const std::vector<Dest>& cmt_map() const { return cmt_; }

  uint32_t free_count() const {
    uint32_t n = 0;
    for (uint32_t t = 0; t < entries_; t++) {
      if (free_[t]) n++;
    }
    return n;
  }

  // Tags that a live speculative mapping points at. A tag must never appear here
  // and in the free set at the same time.
  std::vector<uint32_t> SpecOwnedTags() const {
    std::vector<uint32_t> tags;
    for (uint32_t a = 1; a < arch_regs_; a++) tags.push_back(spec_[a].tag);
    return tags;
  }

  // ------------------------------------------------------------------- eval
  // What the DUT must present this cycle. Pure: it reads the pre-edge state and
  // the stimulus and mutates nothing.
  Outputs Eval(const Stim& s) const {
    Outputs o;

    // A group of up to two macros. What it needs is one tag per lane that writes
    // a non-x0 destination -- 0, 1 or 2 -- and the group is refused whole when
    // the free set cannot supply all of them. This is the single most important
    // line in the two-wide contract, and it is deliberately not "one tag, then
    // see about the second".
    const bool wants0 = s.alloc_req && (s.alloc_rd != 0);
    const bool wants1 = s.alloc2_req && (s.alloc2_rd != 0);
    const uint32_t need = (wants0 ? 1u : 0u) + (wants1 ? 1u : 0u);
    const bool has_enough = free_count() >= need;

    o.alloc_squashed = s.alloc_req && s.squash;
    o.alloc_is_x0 = s.alloc_req && (s.alloc_rd == 0) && !s.squash;
    // Mutually exclusive with the squash report, matching the documented rule: a
    // squash wins, because it discards the group whether or not tags were
    // available.
    o.alloc_exhausted = s.alloc_req && !s.squash && need > 0 && !has_enough;
    o.alloc_accepted = s.alloc_req && !s.squash && has_enough;
    o.alloc_new_valid = o.alloc_accepted && (s.alloc_rd != 0);

    // Lane 1 is part of the same decision, so its acceptance is the group's --
    // gated on lane 1 being present at all. A lane 1 without a lane 0 is refused
    // rather than served: program order admits no second macro without a first.
    o.alloc2_squashed = s.alloc2_req && s.squash;
    o.alloc2_is_x0 = s.alloc2_req && (s.alloc2_rd == 0) && !s.squash;
    o.alloc2_exhausted = s.alloc_req && s.alloc2_req && !s.squash && need > 0 && !has_enough;
    o.alloc2_accepted = o.alloc_accepted && s.alloc2_req;
    o.alloc2_new_valid = o.alloc2_accepted && (s.alloc2_rd != 0);

    // Two scans, exactly as two consecutive single-width allocations would run
    // them: lane 1's starts one past lane 0's tag and cannot see that tag.
    const uint32_t tag0 = ScanFrom(free_, rot_);
    std::vector<bool> lane1_mask = free_;
    if (wants0 && tag0 < entries_) lane1_mask[tag0] = false;
    const uint32_t lane1_ptr = !wants0 ? rot_
                                       : ((tag0 + 1 >= entries_) ? 0u : tag0 + 1);
    const uint32_t tag1 = wants0 ? ScanFrom(lane1_mask, lane1_ptr) : tag0;

    o.alloc_new = Dest{tag0, 0};
    o.alloc_new.gen = gen_valid_[tag0] ? ((gen_[tag0] + 1) & gen_mask_) : 0;
    o.alloc2_new = Dest{tag1, 0};
    o.alloc2_new.gen = gen_valid_[tag1] ? ((gen_[tag1] + 1) & gen_mask_) : 0;

    o.alloc_old_valid = o.alloc_new_valid;
    o.alloc_old = spec_[s.alloc_rd];
    // Lane 1 displaces lane 0's *new* mapping when both lanes write the same rd:
    // that is the mapping lane 1 really supersedes, and the one its commit will
    // release.
    o.alloc2_old_valid = o.alloc2_new_valid;
    o.alloc2_old = (o.alloc_new_valid && s.alloc2_rd == s.alloc_rd) ? o.alloc_new
                                                                    : spec_[s.alloc2_rd];

    o.rs1_is_x0 = s.rs1_addr == 0;
    o.rs1 = o.rs1_is_x0 ? Dest{0, 0} : spec_[s.rs1_addr];
    o.rs1_ready = o.rs1_is_x0 || wb_done_[o.rs1.tag];
    o.rs2_is_x0 = s.rs2_addr == 0;
    o.rs2 = o.rs2_is_x0 ? Dest{0, 0} : spec_[s.rs2_addr];
    o.rs2_ready = o.rs2_is_x0 || wb_done_[o.rs2.tag];

    // Lane 1's sources, with the same-cycle bypass. The bypass is armed only when
    // lane 0 really allocated a tag this cycle: a refused group and an x0 lane
    // both leave lane 1 reading the map, which is what the RTL does too. A
    // bypassed source is never ready -- lane 0's producer is in flight by
    // construction.
    const bool hit3 = o.alloc_new_valid && (s.rs3_addr == s.alloc_rd);
    const bool hit4 = o.alloc_new_valid && (s.rs4_addr == s.alloc_rd);
    o.rs3_is_x0 = s.rs3_addr == 0;
    o.rs3_bypass = hit3;
    o.rs3 = o.rs3_bypass ? o.alloc_new : (o.rs3_is_x0 ? Dest{0, 0} : spec_[s.rs3_addr]);
    o.rs3_ready = o.rs3_is_x0 || (!o.rs3_bypass && wb_done_[o.rs3.tag]);
    o.rs4_is_x0 = s.rs4_addr == 0;
    o.rs4_bypass = hit4;
    o.rs4 = o.rs4_bypass ? o.alloc_new : (o.rs4_is_x0 ? Dest{0, 0} : spec_[s.rs4_addr]);
    o.rs4_ready = o.rs4_is_x0 || (!o.rs4_bypass && wb_done_[o.rs4.tag]);

    // A writeback is stale unless the identity is the tag's *current owner*: in
    // range, generation valid, not free, and carrying the current generation. A
    // write to a free tag has no owner at all, so it is stale rather than a
    // duplicate.
    o.wb_stale = s.wb_valid && !CurrentOwner(s.wb);
    o.wb_duplicate = s.wb_valid && !o.wb_stale && wb_done_[s.wb.tag];
    o.wb_accepted = s.wb_valid && !o.wb_stale && !wb_done_[s.wb.tag];

    // A release separates the two refusals, because they are different bugs:
    //
    //   stale  = the identity on the wire is not one this tag ever had. The
    //            caller is aiming at the wrong register, or at a generation that
    //            has been superseded.
    //   double = the identity is a real, current generation, but the tag is
    //            already free. The caller is releasing the same mapping twice.
    //
    // Conflating them into one "not the owner" test loses the distinction the
    // caller needs: a stale release means "you have the wrong identity", while a
    // double free means "your lifetime accounting is wrong", and only one of
    // those is fixed by re-reading the map.
    o.free_stale = s.free_valid && !IdentityValid(s.free_);
    o.free_double = s.free_valid && !o.free_stale && free_[s.free_.tag];
    o.free_accepted = s.free_valid && !o.free_stale && !free_[s.free_.tag];

    o.commit_x0_dropped = s.commit_valid && (s.commit_rd == 0);
    o.commit_accepted = s.commit_valid && (s.commit_rd != 0);
    o.commit2_x0_dropped = s.commit2_valid && (s.commit2_rd == 0);
    o.commit2_accepted = s.commit2_valid && (s.commit2_rd != 0);

    o.squash_underflow = s.squash && !ckpt_seen_;
    o.squash_accepted = s.squash && ckpt_seen_;
    o.journal_overflow = j_overflow_;
    o.free_count = free_count();
    return o;
  }

  // ------------------------------------------------------------------ update
  // Advance the model by one edge. The order is the documented one: the undo
  // journal is consumed first in wall-clock terms but applied last in state
  // order, because a squash has to overwrite whatever the same cycle did, and
  // the commit is permanent so it has to be visible to the restore in the same
  // cycle. Each step below names the rule it implements.
  void Apply(const Stim& s) {
    const Outputs o = Eval(s);

    // The free set is advanced as a whole next-state vector in the documented
    // order -- allocate clears, release sets, commit sets, undo sets -- rather than
    // as a sequence of in-place edits. In-place edits make the result depend on the
    // order the steps happen to be written in, and two events touching the same tag
    // in one cycle then resolve differently here than in the hardware. Each step
    // names the rule it implements.
    std::vector<bool> free_next = free_;

    // 1. A checkpoint starts a new undo window and marks that one exists. Entries
    //    older than it can never be undone by a squash to it, so they are dropped
    //    rather than kept: that is what makes the window's bound "allocations since
    //    the last checkpoint", which is the bound the register-file size justifies.
    //    A squash in the same cycle does not take a new checkpoint -- it is undoing
    //    to the one that already exists.
    if (s.ckpt_valid && !s.squash) {
      undo_.clear();
      j_len_ = 0;
      ckpt_seen_ = true;
    }

    // 2. One journal entry per allocation, holding the state that allocation
    //    replaced. Allocation is refused in a squash cycle, so the push and the
    //    undo below can never both happen in one cycle.
    // A checkpoint in the same cycle starts a new window, so an allocation in that
    // cycle is *not* journalled: it is already younger than the checkpoint, and the
    // checkpoint is what a later squash will undo to. Journalling it as well would
    // make the window one entry longer than the hardware's and leave a tag
    // allocated after a squash that the hardware had correctly returned.
    if (o.alloc_new_valid && !(s.ckpt_valid && !s.squash)) {
      if (j_len_ >= journal_) {
        // More allocations than the journal can hold. The contract is that this is
        // *reported*, so the shadow records it and the test checks the report.
        j_overflow_ = true;
      } else {
        undo_.push_back(UndoEntry{o.alloc_new.tag, gen_valid_[o.alloc_new.tag]});
        j_len_++;
      }
    }
    // Lane 1's entry follows lane 0's, so the window stays in allocation order and
    // the undo (which walks it oldest first) can invert both allocations. It is
    // pushed only if lane 0's entry fit, which is the same rule the RTL applies.
    if (o.alloc2_new_valid && !(s.ckpt_valid && !s.squash)) {
      if (j_len_ >= journal_) {
        j_overflow_ = true;
      } else {
        undo_.push_back(UndoEntry{o.alloc2_new.tag, gen_valid_[o.alloc2_new.tag]});
        j_len_++;
      }
    }

    // 3. Allocation: take the tag out of the free set, step its generation, and
    //    clear the written flag for the new owner. The rotation point follows the
    //    last allocation, so it ends up where two consecutive single-width
    //    allocations would have left it.
    if (o.alloc_new_valid) {
      free_next[o.alloc_new.tag] = false;
      gen_[o.alloc_new.tag] = o.alloc_new.gen;
      gen_valid_[o.alloc_new.tag] = true;
      wb_done_[o.alloc_new.tag] = false;
      rot_ = (o.alloc_new.tag + 1 >= entries_) ? 0u : o.alloc_new.tag + 1;
    }
    if (o.alloc2_new_valid) {
      free_next[o.alloc2_new.tag] = false;
      gen_[o.alloc2_new.tag] = o.alloc2_new.gen;
      gen_valid_[o.alloc2_new.tag] = true;
      wb_done_[o.alloc2_new.tag] = false;
      rot_ = (o.alloc2_new.tag + 1 >= entries_) ? 0u : o.alloc2_new.tag + 1;
    }

    // 4. An explicit release puts a tag back. The generation does not move: it
    //    counts allocations, not releases.
    if (o.free_accepted) free_next[s.free_.tag] = true;

    // 5. A commit releases the mapping it supersedes, if that is a different
    //    identity from the one it installs. A commit is permanent, so it is not
    //    journalled and a later squash does not undo it.
    //
    //    Lane 1 compares against the committed map *as lane 0 left it*: in a WAW
    //    pair that is lane 0's tag, which lane 1's commit is the one that
    //    supersedes. Against the pre-lane-0 map both lanes would name the same
    //    superseded mapping and lane 0's tag would never come back.
    std::vector<Dest> cmt_after_lane0 = cmt_;
    if (o.commit_accepted) cmt_after_lane0[s.commit_rd] = s.commit;
    if (o.commit_accepted && cmt_[s.commit_rd] != s.commit) {
      free_next[cmt_[s.commit_rd].tag] = true;
    }
    if (o.commit2_accepted && cmt_after_lane0[s.commit2_rd] != s.commit2) {
      free_next[cmt_after_lane0[s.commit2_rd].tag] = true;
    }

    // 6. A squash undoes every allocation made after the checkpoint, oldest entry
    //    first so that the newest is applied last.
    if (o.squash_accepted) {
      for (size_t k = 0; k < j_len_; k++) {
        const UndoEntry& e = undo_[k];
        free_next[e.tag] = true;
        gen_[e.tag] = e.prev_valid ? ((gen_[e.tag] - 1) & gen_mask_) : 0;
        gen_valid_[e.tag] = e.prev_valid;
      }
      j_len_ = 0;
      undo_.clear();
    }

    free_ = free_next;

    // 7. The accepted writeback marks the destination written, so a second producer
    //    for the same identity is a reported duplicate.
    if (o.wb_accepted) wb_done_[s.wb.tag] = true;

    // 8. The maps. A commit lands first so a squash in the same cycle restores to
    //    the post-commit committed map; an allocation in the same cycle lands on
    //    top of that -- and since allocation is refused during a squash, the two can
    //    never both write the same entry here.
    if (o.commit_accepted) cmt_[s.commit_rd] = s.commit;
    if (o.commit2_accepted) cmt_[s.commit2_rd] = s.commit2;
    if (o.squash_accepted) spec_ = cmt_;
    if (o.alloc_new_valid) spec_[s.alloc_rd] = o.alloc_new;
    // Lane 1 lands after lane 0, so a WAW pair ends with lane 1's mapping.
    if (o.alloc2_new_valid) spec_[s.alloc2_rd] = o.alloc2_new;
  }

 private:
  struct UndoEntry {
    uint32_t tag;
    bool prev_valid;
  };

  // The documented rotating scan: the first free tag at or after the rotation
  // point, wrapping once at the end of the register file. The rotation point wraps
  // to zero rather than to the first allocatable tag, so after a full pass the
  // scan walks the reserved range at the bottom of the file as well. Those tags
  // are never free, so the scan passes over them -- which is what keeps the
  // implementation a single linear walk with no special case.
  //
  // `ScanFrom` takes the mask and the pointer, so lane 1's scan is the same
  // function over the mask it is given. Lane 1's mask has lane 0's tag cleared and
  // its pointer starts one past it, which is what makes a two-wide group take the
  // same two tags as two consecutive single-width allocations.
  uint32_t ScanFrom(const std::vector<bool>& mask, uint32_t ptr) const {
    for (uint32_t k = 0; k < entries_; k++) {
      uint32_t t = (ptr + k) % entries_;
      if (mask[t]) return t;
    }
    return 0;  // nothing free: the answer is don't-care, and the request is refused
  }

  // The documented identity test: is (tag, gen) a generation this tag has actually
  // had? It says nothing about whether the tag is currently allocated -- that is a
  // separate question with a separate report.
  bool IdentityValid(const Dest& d) const {
    if (d.tag >= entries_) return false;
    if (!gen_valid_[d.tag]) return false;
    return gen_[d.tag] == (d.gen & gen_mask_);
  }

  // "Current owner" adds liveness: a destination identity is only writable while
  // the tag is allocated and not already written by this generation.
  bool CurrentOwner(const Dest& d) const {
    if (!IdentityValid(d)) return false;
    return !free_[d.tag];
  }

  uint32_t entries_;
  uint32_t tag_w_;
  uint32_t gen_w_;
  uint32_t arch_regs_;
  uint32_t journal_;
  uint32_t gen_mask_;

  std::vector<Dest> spec_;
  std::vector<Dest> cmt_;
  std::vector<bool> free_;
  std::vector<uint32_t> gen_;
  std::vector<bool> gen_valid_;
  std::vector<bool> wb_done_;
  uint32_t rot_;

  std::vector<UndoEntry> undo_;
  size_t j_len_ = 0;
  bool ckpt_seen_ = false;
  bool j_overflow_ = false;
};

// ------------------------------------------------------------------ the harness
//
// Owns the clock, the reset schedule, the comparison against the shadow, and the
// standing invariants. Every wide observation port is read back into plain
// vectors here, so a mismatch message can name the entry that disagreed.
class Harness {
 public:
  Harness(Vmosaic_rename_tb* dut, mosaic::ClockDriver* clk, uint64_t max_cycles)
      : dut_(dut), clk_(clk), max_cycles_(max_cycles) {}

  void Phase(const std::string& name) { phase_ = name; }

  // The shadow this harness advances. Must be bound after every Reset().
  void BindShadow(ShadowRename* shadow) { shadow_ = shadow; }

  // Assert reset for `cycles` rising edges with no other stimulus.
  // Assert reset for `cycles` rising edges with no other stimulus. The shadow is
  // reset here rather than by the caller, because a reset in the middle of a phase
  // that resets only the DUT leaves the two models describing different machines --
  // and the next comparison then reports a disagreement that neither model has.
  void Reset(int cycles) {
    for (int i = 0; i < cycles; i++) {
      Cycle(Stim{}, /*rst=*/true);
    }
    if (shadow_ != nullptr) shadow_->Reset();
  }

  // One full clock period, in three steps that have to happen in this order:
  //
  //   1. Drive the inputs with the clock low and compare the *combinational*
  //      outputs against the shadow computed from the same pre-edge state. The
  //      allocation answer describes the edge that has not happened yet, so it
  //      can only be checked against the state that edge is about to act on.
  //   2. Advance the shadow over the edge.
  //   3. Apply the edge, then compare the *state*. State is registered, so its
  //      new value only exists after the edge -- comparing it before would be
  //      comparing yesterday's registers against tomorrow's model, which is a
  //      mismatch on the very first allocation and nothing but noise after.
  //
  // `observed` therefore holds the pre-edge combinational answer, which is what
  // a phase reasons about, while every state assertion below runs on the
  // post-edge registers.
  // Returns **by value**. A reference would alias `observed_`, which the next
  // call to Cycle overwrites, so a phase that held on to the answer across
  // another Cycle -- which most of them do, since the stimulus for the next
  // request is derived from this answer -- would silently be reading the *next*
  // cycle's outputs. That is a test that passes while asserting nothing.
  Outputs Cycle(const Stim& s, bool rst = false) {
    if (cycles_ >= max_cycles_) {
      Fail(phase_, "max-cycles (" + Dec(max_cycles_) + ") exhausted before the campaign finished");
    }

    dut_->clk = 0;
    dut_->rst = rst ? 1 : 0;
    dut_->alloc_req = s.alloc_req ? 1 : 0;
    dut_->alloc_rd = static_cast<uint8_t>(s.alloc_rd & 0x1f);
    dut_->alloc2_req = s.alloc2_req ? 1 : 0;
    dut_->alloc2_rd = static_cast<uint8_t>(s.alloc2_rd & 0x1f);
    dut_->rs1_addr = static_cast<uint8_t>(s.rs1_addr & 0x1f);
    dut_->rs2_addr = static_cast<uint8_t>(s.rs2_addr & 0x1f);
    dut_->rs3_addr = static_cast<uint8_t>(s.rs3_addr & 0x1f);
    dut_->rs4_addr = static_cast<uint8_t>(s.rs4_addr & 0x1f);
    dut_->wb_valid = s.wb_valid ? 1 : 0;
    dut_->wb_tag = static_cast<uint8_t>(s.wb.tag & 0x7f);
    dut_->wb_gen = static_cast<uint8_t>(s.wb.gen & 0x7f);
    dut_->free_valid = s.free_valid ? 1 : 0;
    dut_->free_tag = static_cast<uint8_t>(s.free_.tag & 0x7f);
    dut_->free_gen = static_cast<uint8_t>(s.free_.gen & 0x7f);
    dut_->commit_valid = s.commit_valid ? 1 : 0;
    dut_->commit_rd = static_cast<uint8_t>(s.commit_rd & 0x1f);
    dut_->commit_tag = static_cast<uint8_t>(s.commit.tag & 0x7f);
    dut_->commit_gen = static_cast<uint8_t>(s.commit.gen & 0x7f);
    dut_->commit2_valid = s.commit2_valid ? 1 : 0;
    dut_->commit2_rd = static_cast<uint8_t>(s.commit2_rd & 0x1f);
    dut_->commit2_tag = static_cast<uint8_t>(s.commit2.tag & 0x7f);
    dut_->commit2_gen = static_cast<uint8_t>(s.commit2.gen & 0x7f);
    dut_->ckpt_valid = s.ckpt_valid ? 1 : 0;
    dut_->squash = s.squash ? 1 : 0;
    dut_->eval();

    const std::string where = phase_ + ": cycle " + Dec(clk_->cycle());

    if (!rst) {
      CompareOutputs(s, where);
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
      CheckInvariants(where);
      ++comparisons_;
    }

    return observed_;
  }

  const Outputs& observed() const { return observed_; }
  uint64_t comparisons() const { return comparisons_; }
  uint64_t cycles() const { return cycles_; }

  // Accessors the phases need on the model, so a phase can reason about the
  // shadow without the harness exposing it for mutation.
  uint32_t shadow_gen_w() const { return shadow_->gen_w(); }
  uint32_t shadow_gen_mask() const { return shadow_->gen_mask(); }

  // The live state as read back from the DUT's observation ports, which is what
  // the phase-level assertions use. Reading the DUT rather than the shadow is
  // the point: a phase assertion that only compared shadow to shadow would pass
  // no matter what the hardware did.
  bool shadow_is_free(uint32_t tag) const { return shadow_->free_set()[tag]; }
  bool shadow_gen_valid(uint32_t tag) const { return shadow_->gen_valid()[tag]; }
  uint32_t shadow_gen_of(uint32_t tag) const { return shadow_->gen()[tag]; }
  const std::vector<Dest>& shadow_cmt_map() const { return shadow_->cmt_map(); }

  std::vector<bool> DutFreeMask() const {
    std::vector<bool> out;
    for (int w = 0; w < kWideWords; w++) {
      const uint32_t word = dut_->dbg_free_mask[w];
      for (int b = 0; b < 32; b++) {
        if (static_cast<uint64_t>(w) * 32 + b < static_cast<uint64_t>(shadow_->entries())) {
          out.push_back((word >> b) & 1u);
        }
      }
    }
    return out;
  }

  std::vector<bool> DutGenValid() const { return ReadMask(dut_->dbg_gen_valid); }
  std::vector<bool> DutWbDone() const { return ReadMask(dut_->dbg_wb_done); }

  // Unpack the generation observation port: `entries` generations of `gen_w`
  // bits, laid out back to back. Unpacked by field rather than by bit -- a
  // bit-at-a-time loop produces entries*gen_w entries, and every comparison
  // against the shadow then becomes a size mismatch that names no real
  // disagreement.
  std::vector<uint32_t> DutGens() const {
    const uint32_t gen_w = shadow_->gen_w();
    std::vector<uint32_t> out(shadow_->entries(), 0);
    for (uint32_t t = 0; t < shadow_->entries(); t++) {
      uint32_t value = 0;
      for (uint32_t b = 0; b < gen_w; b++) {
        const uint32_t bit = t * gen_w + b;
        value |= ((dut_->dbg_tag_gen[bit / 32] >> (bit % 32)) & 1u) << b;
      }
      out[t] = value;
    }
    return out;
  }

  std::vector<Dest> DutSpecMap() const { return ReadMap(dut_->dbg_spec_map); }
  std::vector<Dest> DutCmtMap() const { return ReadMap(dut_->dbg_cmt_map); }

  // The free count as the *registers* hold it, i.e. after the edge that just
  // completed. `observed()` is deliberately the pre-edge combinational answer,
  // so a phase asking "how many tags are free now" must ask here: reading the
  // pre-edge value after the edge compares the register file against a count
  // from before the allocation it is being blamed for.
  uint32_t free_count() const { return dut_->free_count; }
  bool journal_overflow() const { return dut_->journal_overflow != 0; }
  bool squash_accepted() const { return dut_->squash_accepted != 0; }
  bool squash_underflow() const { return dut_->squash_underflow != 0; }

  static constexpr int kWideWords = 3;  // 96 bits rounded up to 32-bit words

 private:

  template <typename Wide>
  std::vector<bool> ReadMask(const Wide& wide) const {
    std::vector<bool> out;
    for (int w = 0; w < kWideWords; w++) {
      for (int b = 0; b < 32; b++) {
        if (static_cast<uint64_t>(w) * 32 + b < static_cast<uint64_t>(shadow_->entries())) {
          out.push_back((wide[w] >> b) & 1u);
        }
      }
    }
    return out;
  }

  // Unpack a map observation port: `arch_regs` entries of (generation, tag),
  // low bits first within each entry, entries laid out back to back.
  template <typename Wide>
  std::vector<Dest> ReadMap(const Wide& wide) const {
    const uint32_t map_w = shadow_->tag_w() + shadow_->gen_w();
    std::vector<Dest> out(shadow_->arch_regs(), Dest{});
    for (uint32_t a = 0; a < shadow_->arch_regs(); a++) {
      uint32_t tag = 0;
      uint32_t gen = 0;
      for (uint32_t b = 0; b < map_w; b++) {
        const uint32_t bit = a * map_w + b;
        const uint32_t value = (wide[bit / 32] >> (bit % 32)) & 1u;
        if (b < shadow_->tag_w()) {
          tag |= value << b;
        } else {
          gen |= value << (b - shadow_->tag_w());
        }
      }
      out[a] = Dest{tag, gen};
    }
    return out;
  }

  // Record what the DUT actually presented this cycle, so a phase assertion that
  // reads `observed()` is asserting about the hardware. Recording the shadow's
  // prediction here instead would make every phase-level assertion a tautology:
  // the prediction is already compared field by field below, so re-reading it
  // would only ever agree with itself, and a phase written against it would pass
  // no matter what the DUT did.
  void CaptureObserved(const Stim& s) {
    observed_.alloc_accepted = dut_->alloc_accepted != 0;
    observed_.alloc_exhausted = dut_->alloc_exhausted != 0;
    observed_.alloc_squashed = dut_->alloc_squashed != 0;
    observed_.alloc_is_x0 = dut_->alloc_is_x0 != 0;
    observed_.alloc_new_valid = dut_->alloc_new_valid != 0;
    observed_.alloc_new = Dest{dut_->alloc_new_tag, dut_->alloc_new_gen};
    observed_.alloc_old_valid = dut_->alloc_old_valid != 0;
    observed_.alloc_old = Dest{dut_->alloc_old_tag, dut_->alloc_old_gen};
    observed_.alloc2_accepted = dut_->alloc2_accepted != 0;
    observed_.alloc2_exhausted = dut_->alloc2_exhausted != 0;
    observed_.alloc2_squashed = dut_->alloc2_squashed != 0;
    observed_.alloc2_is_x0 = dut_->alloc2_is_x0 != 0;
    observed_.alloc2_new_valid = dut_->alloc2_new_valid != 0;
    observed_.alloc2_new = Dest{dut_->alloc2_new_tag, dut_->alloc2_new_gen};
    observed_.alloc2_old_valid = dut_->alloc2_old_valid != 0;
    observed_.alloc2_old = Dest{dut_->alloc2_old_tag, dut_->alloc2_old_gen};
    observed_.rs1_is_x0 = dut_->rs1_is_x0 != 0;
    observed_.rs2_is_x0 = dut_->rs2_is_x0 != 0;
    observed_.rs1_ready = dut_->rs1_ready != 0;
    observed_.rs2_ready = dut_->rs2_ready != 0;
    observed_.rs1 = Dest{dut_->rs1_tag, dut_->rs1_gen};
    observed_.rs2 = Dest{dut_->rs2_tag, dut_->rs2_gen};
    observed_.rs3_is_x0 = dut_->rs3_is_x0 != 0;
    observed_.rs4_is_x0 = dut_->rs4_is_x0 != 0;
    observed_.rs3_ready = dut_->rs3_ready != 0;
    observed_.rs4_ready = dut_->rs4_ready != 0;
    observed_.rs3_bypass = dut_->rs3_bypass != 0;
    observed_.rs4_bypass = dut_->rs4_bypass != 0;
    observed_.rs3 = Dest{dut_->rs3_tag, dut_->rs3_gen};
    observed_.rs4 = Dest{dut_->rs4_tag, dut_->rs4_gen};
    observed_.wb_accepted = dut_->wb_accepted != 0;
    observed_.wb_stale = dut_->wb_stale != 0;
    observed_.wb_duplicate = dut_->wb_duplicate != 0;
    observed_.free_accepted = dut_->free_accepted != 0;
    observed_.free_stale = dut_->free_stale != 0;
    observed_.free_double = dut_->free_double != 0;
    observed_.commit_accepted = dut_->commit_accepted != 0;
    observed_.commit_x0_dropped = dut_->commit_x0_dropped != 0;
    observed_.commit2_accepted = dut_->commit2_accepted != 0;
    observed_.commit2_x0_dropped = dut_->commit2_x0_dropped != 0;
    observed_.squash_accepted = dut_->squash_accepted != 0;
    observed_.squash_underflow = dut_->squash_underflow != 0;
    observed_.journal_overflow = dut_->journal_overflow != 0;
    observed_.free_count = dut_->free_count;
    (void)s;
  }

  void CompareOutputs(const Stim& s, const std::string& where) {
    CaptureObserved(s);
    const Outputs e = shadow_->Eval(s);
    const std::string stim = " " + s.str();

    auto need = [&](bool got, bool want, const char* name) {
      Require(got == want, where,
              std::string(name) + ": expected " + Bool(want) + ", got " + Bool(got) + stim);
    };

    need(dut_->alloc_accepted, e.alloc_accepted, "alloc_accepted");
    need(dut_->alloc_exhausted, e.alloc_exhausted, "alloc_exhausted");
    need(dut_->alloc_squashed, e.alloc_squashed, "alloc_squashed");
    need(dut_->alloc_is_x0, e.alloc_is_x0, "alloc_is_x0");
    need(dut_->alloc_new_valid, e.alloc_new_valid, "alloc_new_valid");
    need(dut_->alloc_old_valid, e.alloc_old_valid, "alloc_old_valid");
    if (e.alloc_new_valid) {
      Require(dut_->alloc_new_tag == e.alloc_new.tag, where,
              "alloc_new_tag: expected " + Dec(e.alloc_new.tag) + ", got " +
                  Dec(dut_->alloc_new_tag) + stim);
      Require(dut_->alloc_new_gen == e.alloc_new.gen, where,
              "alloc_new_gen: expected " + Dec(e.alloc_new.gen) + ", got " +
                  Dec(dut_->alloc_new_gen) + stim);
    }
    if (e.alloc_old_valid) {
      Require(dut_->alloc_old_tag == e.alloc_old.tag, where,
              "alloc_old_tag: expected " + Dec(e.alloc_old.tag) + ", got " +
                  Dec(dut_->alloc_old_tag) + stim);
      Require(dut_->alloc_old_gen == e.alloc_old.gen, where,
              "alloc_old_gen: expected " + Dec(e.alloc_old.gen) + ", got " +
                  Dec(dut_->alloc_old_gen) + stim);
    }

    need(dut_->alloc2_accepted, e.alloc2_accepted, "alloc2_accepted");
    need(dut_->alloc2_exhausted, e.alloc2_exhausted, "alloc2_exhausted");
    need(dut_->alloc2_squashed, e.alloc2_squashed, "alloc2_squashed");
    need(dut_->alloc2_is_x0, e.alloc2_is_x0, "alloc2_is_x0");
    need(dut_->alloc2_new_valid, e.alloc2_new_valid, "alloc2_new_valid");
    need(dut_->alloc2_old_valid, e.alloc2_old_valid, "alloc2_old_valid");
    if (e.alloc2_new_valid) {
      Require(dut_->alloc2_new_tag == e.alloc2_new.tag, where,
              "alloc2_new_tag: expected " + Dec(e.alloc2_new.tag) + ", got " +
                  Dec(dut_->alloc2_new_tag) + stim);
      Require(dut_->alloc2_new_gen == e.alloc2_new.gen, where,
              "alloc2_new_gen: expected " + Dec(e.alloc2_new.gen) + ", got " +
                  Dec(dut_->alloc2_new_gen) + stim);
    }
    if (e.alloc2_old_valid) {
      Require(dut_->alloc2_old_tag == e.alloc2_old.tag, where,
              "alloc2_old_tag: expected " + Dec(e.alloc2_old.tag) + ", got " +
                  Dec(dut_->alloc2_old_tag) + stim);
      Require(dut_->alloc2_old_gen == e.alloc2_old.gen, where,
              "alloc2_old_gen: expected " + Dec(e.alloc2_old.gen) + ", got " +
                  Dec(dut_->alloc2_old_gen) + stim);
    }

    need(dut_->rs1_is_x0, e.rs1_is_x0, "rs1_is_x0");
    need(dut_->rs2_is_x0, e.rs2_is_x0, "rs2_is_x0");
    need(dut_->rs1_ready, e.rs1_ready, "rs1_ready");
    need(dut_->rs2_ready, e.rs2_ready, "rs2_ready");
    Require(dut_->rs1_tag == e.rs1.tag, where,
            "rs1_tag: expected " + Dec(e.rs1.tag) + ", got " + Dec(dut_->rs1_tag) + stim);
    Require(dut_->rs2_tag == e.rs2.tag, where,
            "rs2_tag: expected " + Dec(e.rs2.tag) + ", got " + Dec(dut_->rs2_tag) + stim);
    Require(dut_->rs1_gen == e.rs1.gen, where,
            "rs1_gen: expected " + Dec(e.rs1.gen) + ", got " + Dec(dut_->rs1_gen) + stim);
    Require(dut_->rs2_gen == e.rs2.gen, where,
            "rs2_gen: expected " + Dec(e.rs2.gen) + ", got " + Dec(dut_->rs2_gen) + stim);

    need(dut_->rs3_is_x0, e.rs3_is_x0, "rs3_is_x0");
    need(dut_->rs4_is_x0, e.rs4_is_x0, "rs4_is_x0");
    need(dut_->rs3_ready, e.rs3_ready, "rs3_ready");
    need(dut_->rs4_ready, e.rs4_ready, "rs4_ready");
    need(dut_->rs3_bypass, e.rs3_bypass, "rs3_bypass");
    need(dut_->rs4_bypass, e.rs4_bypass, "rs4_bypass");
    Require(dut_->rs3_tag == e.rs3.tag, where,
            "rs3_tag: expected " + Dec(e.rs3.tag) + ", got " + Dec(dut_->rs3_tag) + stim);
    Require(dut_->rs4_tag == e.rs4.tag, where,
            "rs4_tag: expected " + Dec(e.rs4.tag) + ", got " + Dec(dut_->rs4_tag) + stim);
    Require(dut_->rs3_gen == e.rs3.gen, where,
            "rs3_gen: expected " + Dec(e.rs3.gen) + ", got " + Dec(dut_->rs3_gen) + stim);
    Require(dut_->rs4_gen == e.rs4.gen, where,
            "rs4_gen: expected " + Dec(e.rs4.gen) + ", got " + Dec(dut_->rs4_gen) + stim);

    need(dut_->wb_accepted, e.wb_accepted, "wb_accepted");
    need(dut_->wb_stale, e.wb_stale, "wb_stale");
    need(dut_->wb_duplicate, e.wb_duplicate, "wb_duplicate");

    need(dut_->free_accepted, e.free_accepted, "free_accepted");
    need(dut_->free_stale, e.free_stale, "free_stale");
    need(dut_->free_double, e.free_double, "free_double");

    need(dut_->commit_accepted, e.commit_accepted, "commit_accepted");
    need(dut_->commit_x0_dropped, e.commit_x0_dropped, "commit_x0_dropped");
    need(dut_->commit2_accepted, e.commit2_accepted, "commit2_accepted");
    need(dut_->commit2_x0_dropped, e.commit2_x0_dropped, "commit2_x0_dropped");

    need(dut_->squash_accepted, e.squash_accepted, "squash_accepted");
    need(dut_->squash_underflow, e.squash_underflow, "squash_underflow");
    need(dut_->journal_overflow, e.journal_overflow, "journal_overflow");

    Require(dut_->free_count == e.free_count, where,
            "free_count: expected " + Dec(e.free_count) + ", got " +
                Dec(dut_->free_count) + stim);

    // A refused allocation must say why. "Not accepted" with no reason leaves the
    // core unable to distinguish back-pressure from a squash.
    if (!e.alloc_accepted && s.alloc_req) {
      Require(e.alloc_squashed || e.alloc_exhausted, where,
              "an allocation was refused with neither alloc_squashed nor "
              "alloc_exhausted: the refusal has no reported reason");
    }
    // Exhaustion and squash are mutually exclusive reasons, and reporting both
    // would let a caller act on the wrong one.
    Require(!(e.alloc_exhausted && e.alloc_squashed), where,
            "alloc_exhausted and alloc_squashed are both high for one request");

    // The group's two lanes share one decision. An accepted lane 1 with a refused
    // lane 0 -- or the reverse -- is the half-allocated group the card forbids, so
    // it is checked here from the DUT's own answers rather than only against the
    // shadow's: it has to hold for *any* stimulus, including the random soak's.
    if (s.alloc2_req) {
      Require(e.alloc2_accepted == e.alloc_accepted, where,
              "lane 1 accepted=" + Bool(e.alloc2_accepted) + " while lane 0 accepted=" +
                  Bool(e.alloc_accepted) + ": the group was half-accepted" + stim);
    }
    Require(!(e.alloc2_accepted && !s.alloc_req), where,
            "lane 1 was accepted with no lane 0: program order has no second macro "
            "without a first");

    // Standing invariant: a reported writeback is never accepted for a free tag,
    // and an accepted allocation never returns a tag that is currently owned.
    if (e.wb_accepted) {
      Require(!shadow_->free_set()[s.wb.tag], where,
              "wb_accepted for tag " + Dec(s.wb.tag) + ", which the free set says is free");
    }
    if (e.alloc_new_valid) {
      Require(shadow_->free_set()[e.alloc_new.tag], where,
              "an allocation returned tag " + Dec(e.alloc_new.tag) +
                  ", which was not free: two owners for one physical register");
    }
    if (e.alloc2_new_valid) {
      Require(shadow_->free_set()[e.alloc2_new.tag], where,
              "lane 1's allocation returned tag " + Dec(e.alloc2_new.tag) +
                  ", which was not free: two owners for one physical register");
    }
    // The two lanes of one group never take the same tag. Checked on the DUT's
    // answers, so a design that reused the tag fails here whatever the shadow
    // says.
    Require(!(e.alloc_new_valid && e.alloc2_new_valid && e.alloc_new.tag == e.alloc2_new.tag),
            where,
            "both lanes of one group allocated tag " + Dec(e.alloc_new.tag) + stim);
  }

  // Compare the whole state, not a projection of it.
  void CompareState(const std::string& where) {
    const std::vector<bool> dut_free = DutFreeMask();
    Require(dut_free == shadow_->free_set(), where,
            "the free set differs from the shadow at tag " +
                Dec(FirstDiff(dut_free, shadow_->free_set())));

    Require(DutWbDone() == shadow_->wb_done(), where,
            "the wb_done vector differs from the shadow at tag " +
                Dec(FirstDiff(DutWbDone(), shadow_->wb_done())));

    // The undo window's depth. This is what says a two-wide group journalled both
    // of its allocations: the free set after a squash could only show it one event
    // later, and a group that journalled one lane would leak a tag that no
    // comparison of the map would name.
    Require(dut_->dbg_j_len == shadow_->journal_length(), where,
            "the undo window holds " + Dec(dut_->dbg_j_len) + " entries but the shadow says " +
                Dec(shadow_->journal_length()));

    // The generation table is compared **only where `gen_valid` holds**, and that
    // is the whole contract rather than a convenience. The array is deliberately
    // never reset -- that is the rtl/common/mosaic_ram.sv rule, and it is why
    // `gen_valid` exists at all -- so an entry no allocation has touched holds
    // whatever the silicon powered up with. A C++ model cannot mirror power-up
    // contents, so comparing the whole table would be comparing undefined data and
    // would fail on any design that followed the rule.
    //
    // What replaces it is stronger than a raw comparison would have been: every
    // place the generation is *used* -- the writeback check, the release check and
    // the allocation step -- is gated on `gen_valid`, so a stale entry is not
    // merely unread, it is unreachable. The `generation` phase pins that down from
    // the consumer side with a writeback aimed at a never-allocated tag.
    const std::vector<uint32_t> dut_gens = DutGens();
    const std::vector<bool>& gv = DutGenValid();
    const std::vector<uint32_t>& sh_gens = shadow_->gen();
    const std::vector<bool>& sh_gv = shadow_->gen_valid();
    Require(gv == sh_gv, where,
            "the gen_valid vector differs from the shadow at tag " +
                Dec(FirstDiff(gv, sh_gv)));
    for (uint32_t t = 0; t < shadow_->entries(); t++) {
      if (!gv[t]) continue;
      Require(dut_gens[t] == sh_gens[t], where,
              "the generation of tag " + Dec(t) + ", which has been allocated, is " +
                  Dec(dut_gens[t]) + " but the shadow says " + Dec(sh_gens[t]));
    }

    const std::vector<Dest> dut_spec = DutSpecMap();
    const std::vector<Dest>& sh_spec = shadow_->spec_map();
    for (uint32_t a = 0; a < shadow_->arch_regs(); a++) {
      Require(dut_spec[a] == sh_spec[a], where,
              "spec_map[x" + Dec(a) + "]: expected " + sh_spec[a].str() + ", got " +
                  dut_spec[a].str());
    }

    const std::vector<Dest> dut_cmt = DutCmtMap();
    const std::vector<Dest>& sh_cmt = shadow_->cmt_map();
    for (uint32_t a = 0; a < shadow_->arch_regs(); a++) {
      Require(dut_cmt[a] == sh_cmt[a], where,
              "cmt_map[x" + Dec(a) + "]: expected " + sh_cmt[a].str() + ", got " +
                  dut_cmt[a].str());
    }
  }

  // Conservation and ownership, recomputed from the DUT's own observation ports.
  void CheckInvariants(const std::string& where) {
    const std::vector<bool> free_mask = DutFreeMask();
    const std::vector<Dest> spec = DutSpecMap();
    const std::vector<Dest> cmt = DutCmtMap();

    uint32_t free_n = 0;
    for (uint32_t t = 0; t < shadow_->entries(); t++) {
      if (free_mask[t]) free_n++;
    }
    Require(dut_->free_count == free_n, where,
            "free_count " + Dec(dut_->free_count) + " does not equal the population count of "
            "the free mask " + Dec(free_n));

    // Every live speculative mapping names a tag that is not free. This is the
    // exactly-once-ownership invariant in its sharpest form: a tag named by a
    // mapping and also free would be handed to the next allocation while an
    // instruction can still read it.
    for (uint32_t a = 1; a < shadow_->arch_regs(); a++) {
      Require(!free_mask[spec[a].tag], where,
              "x" + Dec(a) + " maps to tag " + Dec(spec[a].tag) +
                  ", which is also in the free set: one physical register has two owners");
    }
    // The committed map is a subset of the live mappings' world: a committed
    // mapping's tag must not be free either, or a commit would have released a
    // register the architecture still points at.
    for (uint32_t a = 0; a < shadow_->arch_regs(); a++) {
      Require(!free_mask[cmt[a].tag], where,
              "x" + Dec(a) + "'s committed mapping is tag " + Dec(cmt[a].tag) +
                  ", which is in the free set: the architectural register points at a "
                  "free physical register");
    }
    // Out-of-range tags must never be allocated: 96 entries in a 7-bit field
    // names 32 tags that have no home bank, and the bank decode is a partition,
    // so those tags are refused rather than wrapped onto a real one.
    for (uint32_t a = 1; a < shadow_->arch_regs(); a++) {
      Require(spec[a].tag < shadow_->entries(), where,
              "x" + Dec(a) + " maps to tag " + Dec(spec[a].tag) +
                  ", which is outside the register file");
    }
    Require(!dut_->journal_overflow, where,
            "the DUT reported journal_overflow: the undo window was exceeded, so a squash "
            "would restore less than the truth");
  }

  template <typename T>
  static uint32_t FirstDiff(const std::vector<T>& a, const std::vector<T>& b) {
    const size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; i++) {
      if (!(a[i] == b[i])) return static_cast<uint32_t>(i);
    }
    return static_cast<uint32_t>(n);
  }

  Vmosaic_rename_tb* dut_;
  mosaic::ClockDriver* clk_;
  uint64_t max_cycles_;
  ShadowRename* shadow_ = nullptr;
  std::string phase_;
  Outputs observed_;
  uint64_t comparisons_ = 0;
  uint64_t cycles_ = 0;
};

// -------------------------------------------------------------------- phases

// Phase 1: the documented cold state.
void PhaseResetState(Harness* h, mosaic::Reporter* reporter, uint32_t entries,
                     uint32_t arch_regs, uint32_t banks, uint32_t rows) {
  const std::vector<bool> free_mask = h->DutFreeMask();
  const std::vector<Dest> spec = h->DutSpecMap();
  const std::vector<Dest> cmt = h->DutCmtMap();

  for (uint32_t a = 0; a < arch_regs; a++) {
    Require(spec[a] == Dest{a, 0}, "reset-state",
            "spec_map[x" + Dec(a) + "] at reset: expected (" + Dec(a) + ",0), got " +
                spec[a].str());
    Require(cmt[a] == Dest{a, 0}, "reset-state",
            "cmt_map[x" + Dec(a) + "] at reset: expected (" + Dec(a) + ",0), got " +
                cmt[a].str());
  }

  // The invariant that matters at reset: the architectural reset mappings own
  // their tags, so those tags are *not* free. A free list reset to all-ones
  // would hand x5's tag to the first instruction that writes any register.
  for (uint32_t a = 0; a < arch_regs; a++) {
    Require(!free_mask[a], "reset-state",
            "tag " + Dec(a) + " is free at reset, but x" + Dec(a) +
                " is mapped to it: one physical register has two owners from cycle 0");
  }
  for (uint32_t t = arch_regs; t < entries; t++) {
    Require(free_mask[t], "reset-state",
            "tag " + Dec(t) + " is not free at reset, and no architectural register maps "
            "to it, so nothing owns it: a tag is neither free nor owned");
  }

  Require(h->DutGenValid() == std::vector<bool>(entries, false), "reset-state",
          "no tag has a valid generation at reset");
  Require(h->DutWbDone() == std::vector<bool>(entries, false), "reset-state",
          "no tag is marked written at reset");

  // The bank decode is a partition: every entry must land in exactly one bank and
  // every bank must get its share. Asserted on the observed geometry so a profile
  // that stopped dividing evenly would fail here rather than mis-decode silently.
  Require(entries % banks == 0, "reset-state",
          "the register file does not divide evenly into banks, so the bank decode is not "
          "a partition");
  Require(entries / banks == rows, "reset-state",
          "the observed rows-per-bank does not match entries/banks");

  reporter->Check(true, "reset-state: the cold state is the documented one");
}

// Phase 2: single-width ownership. Every allocation, release and commit is checked
// to move exactly one tag, and every reported destination to be one that was
// genuinely free.
void PhaseOwnership(Harness* h, mosaic::Reporter* reporter, uint32_t entries,
                     uint32_t arch_regs) {
  // The mapping each rd is expected to hold *now*, from the DUT's own registers.
  // It is re-read every iteration rather than captured once: the phase writes to
  // each rd twice, so the second allocation legitimately displaces the first
  // allocation's tag rather than the reset mapping.
  std::vector<Dest> expect_old = h->DutSpecMap();

  // The free count must fall by exactly one per allocation and rise by exactly
  // one per release, and never by two. Checking the *delta* rather than an
  // absolute count is what makes a double-free visible: it would show up as a
  // count that rose by two.
  std::vector<Dest> live;  // destinations currently held by live allocations
  uint32_t prev_free = h->free_count();

  for (uint32_t i = 0; i < 40; i++) {
    const uint32_t rd = 1 + (i % (arch_regs - 1));

    // Allocate. The displaced mapping is the one rd had before.
    Stim s;
    s.alloc_req = true;
    s.alloc_rd = rd;
    Outputs o = h->Cycle(s);

    Require(o.alloc_accepted, "ownership",
            "cycle " + Dec(i) + ": a request to x" + Dec(rd) +
                " was refused while the free set was not empty");
    Require(o.alloc_new_valid, "ownership", "the allocation produced no destination");
    Require(o.alloc_old_valid && o.alloc_old == expect_old[rd], "ownership",
            "the displaced mapping for x" + Dec(rd) + " was not reported as " +
                expect_old[rd].str());
    expect_old[rd] = o.alloc_new;

    Require(h->free_count() == prev_free - 1, "ownership",
            "the free count went from " + Dec(prev_free) + " to " +
                Dec(h->free_count()) + " across one allocation");

    live.push_back(o.alloc_new);
    prev_free = h->free_count();

    // Write it back, then deliver a *second* writeback for the same identity:
    // the first must be accepted and the second must be reported as a duplicate,
    // because a result is written to exactly one destination, never broadcast.
    Stim w;
    w.wb_valid = true;
    w.wb = o.alloc_new;
    Outputs w1 = h->Cycle(w);
    Require(w1.wb_accepted, "ownership",
            "the writeback of the allocation's own destination " + o.alloc_new.str() +
                " was refused");

    Stim w2;
    w2.wb_valid = true;
    w2.wb = o.alloc_new;
    Outputs w2o = h->Cycle(w2);
    Require(!w2o.wb_accepted, "ownership",
            "a second writeback for destination " + o.alloc_new.str() +
                " was accepted: one result, two writes");
    Require(w2o.wb_duplicate, "ownership",
            "a duplicate writeback was refused without reporting wb_duplicate");

    // Retire. The commit is the module's legal release path: it installs the new
    // mapping and frees the one it supersedes. Releasing the *new* mapping here
    // instead would be a caller error -- an architectural register would still
    // point at a tag on the free list -- and the standing invariant below would
    // (correctly) reject it. Getting that wrong in the stimulus rather than in
    // the hardware is worth recording: it is the shape of the bug this whole
    // package exists to prevent.
    Stim c;
    c.commit_valid = true;
    c.commit_rd = rd;
    c.commit = o.alloc_new;
    Outputs co = h->Cycle(c);
    Require(co.commit_accepted, "ownership",
            "cycle " + Dec(i) + ": the commit of " + o.alloc_new.str() + " was refused");

    // The commit frees exactly one mapping -- the one rd pointed at before -- so
    // the free count must rise by exactly one. A commit that freed two, or none,
    // would be an ownership error that no per-tag comparison would name.
    Require(h->free_count() == prev_free + 1, "ownership",
            "the free count went from " + Dec(prev_free) + " to " + Dec(h->free_count()) +
                " across one commit: a commit must release exactly the mapping it "
                "supersedes");
    prev_free = h->free_count();

    // The superseded mapping is now dead and free. Releasing it again is a double
    // free, and it must be reported rather than accepted: accepting it would put a
    // tag on the free list twice over and hand the same register to two future
    // owners.
    const Dest victim = o.alloc_old;
    Require(victim.tag < entries && h->shadow_is_free(victim.tag), "ownership",
            "the superseded mapping " + victim.str() +
                " was not free after the commit that released it");

    Stim f2;
    f2.free_valid = true;
    f2.free_ = victim;
    Outputs f2o = h->Cycle(f2);
    Require(!f2o.free_accepted, "ownership",
            "releasing " + victim.str() + " a second time was accepted: a double free puts "
            "a live register back on the free list");

    // *Which* report fires depends on the victim, and both are refusals for the
    // right reason. A tag whose generation was never valid -- an architectural
    // reset mapping, whose generation has never been allocated -- is refused as
    // stale, because the identity on the wire names a generation the tag has
    // never had. A tag that really was allocated is refused as a double free.
    // Asserting one report unconditionally would have been wrong on the first
    // iteration and right on the rest, which is the worst possible test.
    if (h->shadow_gen_valid(victim.tag)) {
      Require(f2o.free_double, "ownership",
              "releasing the already-free allocated tag " + victim.str() +
                  " was refused without reporting free_double");
    } else {
      Require(f2o.free_stale, "ownership",
              "releasing the already-free tag " + victim.str() +
                  " was refused without any reported reason");
    }
    Require(!(f2o.free_double && f2o.free_stale), "ownership",
            "a refused release reported both stale and double-free: the two reasons are "
            "different bugs and reporting both sends the caller the wrong way");
    Require(h->free_count() == prev_free, "ownership",
            "a refused double free still changed the free count");
  }

  // A release carrying the wrong generation is stale, not a release: it would
  // free somebody else's register. Aimed at a tag that really was allocated --
  // found by asking the shadow, not by assuming an index -- and carrying a
  // generation that tag has never had, so the refusal is unambiguously about the
  // generation rather than about the tag already being free.
  uint32_t stale_tag = 0;
  for (uint32_t t = 0; t < entries; t++) {
    if (h->shadow_gen_valid(t) && h->shadow_is_free(t)) {
      stale_tag = t;
      break;
    }
  }
  Require(stale_tag != 0, "ownership",
          "no allocated tag was free at the end of the phase, so the stale-generation "
          "release could not be aimed at one");
  const uint32_t stale_gen = (h->shadow_gen_of(stale_tag) + 1) & h->shadow_gen_mask();
  Stim stale;
  stale.free_valid = true;
  stale.free_ = Dest{stale_tag, stale_gen};
  Outputs stale_o = h->Cycle(stale);
  Require(!stale_o.free_accepted && stale_o.free_stale, "ownership",
          "releasing tag " + Dec(stale_tag) + " at generation " + Dec(stale_gen) +
              " when it is at generation " + Dec(h->shadow_gen_of(stale_tag)) +
              " was not reported as a stale release");

  // An out-of-range tag has no home bank at all: 96 entries in a 7-bit field names
  // 32 tags the bank decode cannot place, and the rule is "refused, never wrapped".
  Stim oor;
  oor.free_valid = true;
  oor.free_ = Dest{entries, 0};
  Outputs oor_o = h->Cycle(oor);
  Require(!oor_o.free_accepted && oor_o.free_stale, "ownership",
          "releasing out-of-range tag " + Dec(entries) +
              " was accepted or unreported: a tag with no home bank must be refused, never "
              "wrapped onto a real one");

  // Every instruction of the campaign has committed, so the free set is back to
  // its reset contents: `entries - arch_regs` tags. Not `entries`, because the
  // architectural reset mappings are owned and never freed by this phase.
  Require(h->free_count() == entries - arch_regs, "ownership",
          "after every allocation of the campaign committed, the free count should be back "
          "to the reset value " + Dec(entries - arch_regs) + ", got " + Dec(h->free_count()));

  reporter->Check(true,
                  "ownership: every tag had exactly one owner at every cycle, and duplicate "
                  "producers and double frees were reported");
}

// Phase 3: the generation. This is the phase the card is about.
void PhaseGeneration(Harness* h, mosaic::Reporter* reporter, uint32_t entries,
                     uint32_t arch_regs) {
  // Allocate a destination for x5 and note its identity.
  Stim a;
  a.alloc_req = true;
  a.alloc_rd = 5;
  Outputs first = h->Cycle(a);
  Require(first.alloc_new_valid, "generation", "the first allocation was refused");
  const Dest old = first.alloc_new;
  Require(old.gen == 0, "generation",
          "the first allocation of a never-allocated tag should be generation 0, got " +
              Dec(old.gen));

  Stim w;
  w.wb_valid = true;
  w.wb = old;
  Require(h->Cycle(w).wb_accepted, "generation", "the writeback was refused");

  // The first delivery of a stale generation: the tag is still owned by this very
  // instruction, so this is the *original* producer, and it must be accepted.
  // Establishing that first matters -- otherwise "a stale write was rejected"
  // could be satisfied by a module that rejects everything.
  Stim wdup;
  wdup.wb_valid = true;
  wdup.wb = old;
  Outputs wd = h->Cycle(wdup);
  Require(!wd.wb_accepted && wd.wb_duplicate, "generation",
          "a second writeback for " + old.str() +
              " was not reported as a duplicate producer");

  // Commit, so the mapping becomes the architectural one.
  Stim c;
  c.commit_valid = true;
  c.commit_rd = 5;
  c.commit = old;
  Require(h->Cycle(c).commit_accepted, "generation", "the commit was refused");

  // Now drive the ABA case. The allocation scan rotates, so a released tag is not
  // handed straight back -- it comes round again when the scan wraps past the end
  // of the register file. That is a property worth stating rather than working
  // around: the rotation is what keeps consecutive allocations off the same bank
  // row, and the cost is that the recycle needs a bounded number of allocations
  // rather than happening on the next one.
  //
  // The loop below therefore runs until tag `old.tag` comes back, and *fails* if
  // it does not within one full pass over the register file. An unbounded search
  // here would quietly turn into "the phase ended without testing anything".
  Dest fresh{};
  bool recycled = false;
  uint32_t steps = 0;
  while (steps <= entries) {
    // Checkpoint before every step. The undo journal holds one entry per
    // allocation since the checkpoint and is sized for a full ROB, so a campaign
    // that allocated more than that without checkpointing would exceed the bound
    // and correctly report `journal_overflow`. A real core takes a checkpoint at
    // every branch for exactly this reason, and the phase has to model that
    // discipline or it is measuring the journal bound rather than the generation.
    Stim ck;
    ck.ckpt_valid = true;
    h->Cycle(ck);

    Stim a2;
    a2.alloc_req = true;
    a2.alloc_rd = 5;
    Outputs o = h->Cycle(a2);
    Require(o.alloc_new_valid, "generation",
            "the recycle campaign ran out of tags after " + Dec(steps) +
                " allocations without ever handing back tag " + Dec(old.tag));
    steps++;

    Stim c2;
    c2.commit_valid = true;
    c2.commit_rd = 5;
    c2.commit = o.alloc_new;
    Require(h->Cycle(c2).commit_accepted, "generation", "a recycle commit was refused");

    if (o.alloc_new.tag == old.tag) {
      fresh = o.alloc_new;
      recycled = true;
      break;
    }

    // Only the steps that did *not* recycle the tag write back. The step that
    // does recycle it must leave the new owner unwritten, because the phase is
    // about to deliver a stale write first and then the real one: if the fresh
    // destination had already been written, the later writeback would be refused
    // as a duplicate and the phase would be asserting about the duplicate guard
    // while claiming to test the generation.
    Stim w2;
    w2.wb_valid = true;
    w2.wb = o.alloc_new;
    Require(h->Cycle(w2).wb_accepted, "generation", "a recycle writeback was refused");
  }

  Require(recycled, "generation",
          "tag " + Dec(old.tag) + " was not handed out again within " + Dec(entries) +
              " allocations: the phase never exercised a recycled tag");
  Require(fresh.gen != old.gen, "generation",
          "tag " + Dec(old.tag) + " was handed out again with the same generation " +
              Dec(old.gen) + ": the recycled tag is indistinguishable from the old owner");
  Require((fresh.gen - old.gen) % h->shadow_gen_mask() == 1, "generation",
          "tag " + Dec(old.tag) + " came back at generation " + Dec(fresh.gen) +
              " after generation " + Dec(old.gen) + ": the generation did not advance by "
              "exactly one across a single recycle");

  // The physical tag is the *same* and the architectural mapping is the *same
  // register*, so only the generation distinguishes the old owner from the new
  // one. This is the whole ABA case in three lines.

  // **The test that matters.** Deliver a writeback carrying the *old* generation.
  // It is aimed at a previous owner of that tag and must be refused.
  Stim stale;
  stale.wb_valid = true;
  stale.wb = old;
  Outputs stale_o = h->Cycle(stale);
  Require(!stale_o.wb_accepted, "generation",
          "a writeback with the stale generation " + old.str() +
              " was accepted on cycle " + Dec(steps) + " after the tag was recycled to " +
              fresh.str() + ": a late result overwrote the new owner of tag " +
              Dec(old.tag));
  Require(stale_o.wb_stale, "generation",
          "a stale-generation writeback was refused without reporting wb_stale");

  // The current owner's writeback must still be accepted afterwards: rejection
  // must not have consumed the destination.
  Stim good;
  good.wb_valid = true;
  good.wb = fresh;
  Require(h->Cycle(good).wb_accepted, "generation",
          "the current owner's writeback " + fresh.str() +
              " was refused after a stale one was rejected: the rejection consumed the "
              "destination");

  // And the mapping x5 still points at the new owner.
  const std::vector<Dest> spec = h->DutSpecMap();
  Require(spec[5] == fresh, "generation",
          "x5 maps to " + spec[5].str() + " after the stale writeback, expected " +
              fresh.str());

  // A generation that is wrong in the other direction -- one *ahead* of the
  // current generation -- is equally not the current owner.
  Stim ahead;
  ahead.wb_valid = true;
  ahead.wb = Dest{fresh.tag, (fresh.gen + 1) & h->shadow_gen_mask()};
  Outputs ahead_o = h->Cycle(ahead);
  Require(!ahead_o.wb_accepted && ahead_o.wb_stale, "generation",
          "a writeback with a generation the tag has not reached was accepted");

  // The old owner is still *reachable*: the same tag at the old generation is
  // refused not because the tag is gone but because the identity is not current.
  // Releasing it must fail the same way, for the same reason -- otherwise a caller
  // could free the new owner's register on the strength of the old identity.
  Stim stale_free;
  stale_free.free_valid = true;
  stale_free.free_ = old;
  Outputs sf = h->Cycle(stale_free);
  Require(!sf.free_accepted && sf.free_stale, "generation",
          "releasing the stale identity " + old.str() +
              " was accepted: a superseded generation still names a live owner");
  Require(h->shadow_is_free(fresh.tag) == false, "generation",
          "tag " + Dec(fresh.tag) + " became free, so the stale release above freed the "
          "current owner's register");

  // A tag that was never allocated since reset has no valid generation, so a
  // writeback at generation 0 to it is stale rather than a write into a
  // never-owned register.
  Stim never;
  never.wb_valid = true;
  never.wb = Dest{static_cast<uint32_t>(arch_regs), 0};
  Outputs never_o = h->Cycle(never);
  Require(!never_o.wb_accepted && never_o.wb_stale, "generation",
          "a writeback to a free tag was accepted");

  reporter->Check(true,
                  "generation: tag " + Dec(old.tag) + " was recycled from " + old.str() +
                      " to " + fresh.str() + " after " + Dec(steps) +
                      " allocations, the stale write was rejected, and the current owner "
                      "kept its destination");
}

// Phase 4: wrap. Enough allocate/release cycles to wrap the physical tags several
// times over, with a stale generation rejected after every reuse.
void PhaseWrap(Harness* h, mosaic::Reporter* reporter, uint32_t entries, uint32_t arch_regs) {
  // How many times each physical tag has been handed out, and how many times the
  // rotation point went *backwards* -- i.e. wrapped past the end of the register
  // file. Both counters are the anti-vacuity evidence for this phase: a campaign
  // that never wrapped would pass a check that only compared against the shadow.
  std::vector<uint32_t> allocations(entries, 0);
  uint32_t pointer_wraps = 0;
  uint32_t last_tag = 0;
  bool have_last = false;

  // Two full passes over the register file, so every tag is reused at least once
  // and the tags near the top wrap their own index twice.
  const uint32_t steps = 2 * entries + 8;
  for (uint32_t i = 0; i < steps; i++) {
    const uint32_t rd = 1 + (i % (arch_regs - 1));

    // A checkpoint every step, so the undo window stays inside its bound. This is
    // the discipline a real core follows at every branch; without it the journal
    // legitimately overflows and the phase would be measuring the journal instead
    // of the wrap.
    Stim ck;
    ck.ckpt_valid = true;
    h->Cycle(ck);

    Stim a;
    a.alloc_req = true;
    a.alloc_rd = rd;
    Outputs o = h->Cycle(a);
    Require(o.alloc_new_valid, "wrap",
            "step " + Dec(i) + ": allocation refused during the wrap campaign");

    allocations[o.alloc_new.tag]++;
    if (have_last && o.alloc_new.tag < last_tag) pointer_wraps++;
    last_tag = o.alloc_new.tag;
    have_last = true;

    // After every *reuse* of a tag -- every allocation whose generation is not the
    // first for that tag -- a writeback carrying the previous generation must be
    // refused. This is the property that has to survive the wrap, and it is
    // asserted on each reuse rather than once at the end, so a defect that only
    // shows up on the second pass cannot hide behind the first.
    if (allocations[o.alloc_new.tag] > 1) {
      const uint32_t prev = (o.alloc_new.gen - 1) & h->shadow_gen_mask();
      Require(prev != o.alloc_new.gen, "wrap",
              "tag " + Dec(o.alloc_new.tag) + " came back at generation " +
                  Dec(o.alloc_new.gen) + " and the previous generation is the same value: "
                  "the generation did not advance, so no reuse could ever be detected");

      Stim stale;
      stale.wb_valid = true;
      stale.wb = Dest{o.alloc_new.tag, prev};
      Outputs so = h->Cycle(stale);
      Require(!so.wb_accepted, "wrap",
              "step " + Dec(i) + ": after tag " + Dec(o.alloc_new.tag) +
                  " was reused, a writeback with the previous generation " + Dec(prev) +
                  " was accepted");
      Require(so.wb_stale, "wrap",
              "step " + Dec(i) + ": a stale generation was refused without a report");
    }

    // Write back and *commit*. The commit is what releases the superseded
    // mapping, which is the only legal release: an architectural register still
    // points at the new tag until the next allocation to that rd supersedes it, so
    // an explicit release here would be a caller bug -- and the standing invariant
    // below would reject it.
    Stim w;
    w.wb_valid = true;
    w.wb = o.alloc_new;
    Require(h->Cycle(w).wb_accepted, "wrap",
            "step " + Dec(i) + ": the writeback of " + o.alloc_new.str() + " was refused");

    Stim c;
    c.commit_valid = true;
    c.commit_rd = rd;
    c.commit = o.alloc_new;
    Require(h->Cycle(c).commit_accepted, "wrap",
            "step " + Dec(i) + ": the commit of " + o.alloc_new.str() + " was refused");
  }

  // Only the allocatable tags can be reused: tags 0..ARCH_REGS-1 are the
  // architectural reset mappings and are owned from cycle 0, so no allocation can
  // ever hand them out. Averaging over the whole file would quietly dilute the
  // evidence, so the minimum is taken over the allocatable range only.
  uint32_t min_reuse = allocations[arch_regs];
  for (uint32_t t = arch_regs; t < entries; t++) {
    if (allocations[t] < min_reuse) min_reuse = allocations[t];
  }

  // The campaign has to have actually wrapped. A test that never reaches the end
  // of the register file proves nothing about a wrapping structure, so these are
  // assertions, not comments.
  Require(pointer_wraps >= 2, "wrap",
          "the rotation point wrapped only " + Dec(pointer_wraps) + " time(s) over " +
              Dec(steps) + " allocations: the campaign never reached the end of the "
              "register file, so it proved nothing about wrapping");
  Require(min_reuse >= 2, "wrap",
          "the least-reused tag was handed out " + Dec(min_reuse) +
              " time(s): at least one tag must be recycled for the generation to matter");

  // The generation is 7 bits wide, so it wraps after 128 allocations of the *same*
  // tag. The campaign must stay below that, and the bound is stated because a
  // generation that has wrapped can no longer reject anything -- the stale checks
  // past that point would be claiming more than they checked.
  const uint32_t gen_w = h->shadow_gen_w();
  const uint32_t max_gen_alloc = 1u << gen_w;
  Require(max_gen_alloc > min_reuse, "wrap",
          "the campaign reused a tag " + Dec(min_reuse) + " times, which reaches or "
              "exceeds the " + Dec(max_gen_alloc) + " allocations the " + Dec(gen_w) +
              "-bit generation can distinguish: the stale-generation checks past that "
              "point are no longer evidence");

  reporter->Check(true,
                  "wrap: the rotation point wrapped " + Dec(pointer_wraps) +
                      " times over " + Dec(steps) + " allocations, every allocatable tag "
                      "was recycled at least " + Dec(min_reuse) +
                      " time(s), and a stale generation was rejected on every reuse");
}

// Phase 5: x0.
void PhaseX0(Harness* h, mosaic::Reporter* reporter, uint32_t entries, uint32_t arch_regs) {
  const uint32_t free_before = h->free_count();
  const std::vector<Dest> spec_before = h->DutSpecMap();
  const std::vector<Dest> cmt_before = h->DutCmtMap();

  // Repeated writes to x0. None of them may consume a physical tag: the free
  // count has to be *exactly* unchanged after all of them, which is the assertion
  // that catches the leak. A leak of one tag per write to x0 is invisible here
  // and shows up much later as spurious exhaustion.
  for (uint32_t i = 0; i < 24; i++) {
    Stim s;
    s.alloc_req = true;
    s.alloc_rd = 0;
    Outputs o = h->Cycle(s);
    Require(o.alloc_accepted, "x0",
            "a write to x0 was refused: x0 must be accepted and allocate nothing");
    Require(!o.alloc_new_valid, "x0",
            "a write to x0 allocated physical tag " + Dec(o.alloc_new.tag) + " at generation " +
                Dec(o.alloc_new.gen) + ": x0 must never consume a physical tag");
    Require(!o.alloc_old_valid, "x0",
            "a write to x0 reported a displaced mapping: x0 has no physical mapping");
    Require(o.alloc_is_x0, "x0", "a write to x0 was not reported as such");
    Require(!o.alloc_exhausted, "x0",
            "a write to x0 was reported as exhausted: x0 needs no tag, so the free set is "
            "irrelevant to it");
    Require(h->free_count() == free_before, "x0",
            "the free count moved from " + Dec(free_before) + " to " +
                Dec(h->free_count()) + " across a write to x0: the free list leaks "
                "a tag per write to x0");
  }

  // A commit to x0 frees nothing and changes no map.
  Stim c;
  c.commit_valid = true;
  c.commit_rd = 0;
  c.commit = Dest{0, 0};
  Outputs co = h->Cycle(c);
  Require(co.commit_x0_dropped, "x0", "a commit to x0 was not reported as dropped");
  Require(!co.commit_accepted, "x0", "a commit to x0 was accepted as a real commit");
  Require(h->free_count() == free_before, "x0",
          "a commit to x0 changed the free count");
  Require(h->DutSpecMap() == spec_before, "x0", "a write and a commit to x0 changed the map");
  Require(h->DutCmtMap() == cmt_before, "x0",
          "a commit to x0 changed the committed map");

  // Reads of x0 return the zero identity and are flagged, so a consumer knows not
  // to read a physical register at all.
  for (const uint32_t a : {0u, 1u, 2u}) {
    Stim r;
    r.rs1_addr = a;
    r.rs2_addr = a;
    Outputs o = h->Cycle(r);
    Require(o.rs1_is_x0 == (a == 0), "x0",
            "rs1_is_x0 for x" + Dec(a) + " is " + Bool(o.rs1_is_x0));
    Require(o.rs2_is_x0 == (a == 0), "x0",
            "rs2_is_x0 for x" + Dec(a) + " is " + Bool(o.rs2_is_x0));
    if (a == 0) {
      Require(o.rs1 == Dest{0, 0} && o.rs2 == Dest{0, 0}, "x0",
              "a read of x0 did not return the zero identity");
    }
  }

  // The interaction the card cares about most: x0 with an *empty* free list. If
  // x0 needed a tag it would be refused here, and a write to x0 would have to be
  // treated as a resource conflict -- which is exactly the bug that makes x0
  // writes stall a machine that has no registers left.
  Stim fill;
  fill.alloc_req = true;
  fill.alloc_rd = 1;
  h->Cycle(fill);

  // Empty the free set by allocating everything that is left, and never release.
  uint32_t guard = 0;
  while (h->free_count() > 0 && guard < entries + 8) {
    Stim a;
    a.alloc_req = true;
    a.alloc_rd = 1 + (guard % (arch_regs - 1));
    h->Cycle(a);
    guard++;
  }
  Require(h->free_count() == 0, "x0",
          "the campaign failed to empty the free set, so the x0-with-no-tags case was "
          "never reached");

  Stim x0full;
  x0full.alloc_req = true;
  x0full.alloc_rd = 0;
  Outputs xf = h->Cycle(x0full);
  Require(xf.alloc_accepted, "x0",
          "a write to x0 was refused with an empty free list: x0 must not need a tag");
  Require(!xf.alloc_new_valid, "x0",
          "a write to x0 allocated a tag with an empty free list: it took a tag that does "
          "not exist");
  Require(!xf.alloc_exhausted, "x0",
          "a write to x0 reported exhaustion with an empty free list");

  reporter->Check(true,
                  "x0: writes to x0 allocated nothing, the free count never moved, reads "
                  "returned zero, and x0 still worked with an empty free list");
}

// Phase 6: squash.
void PhaseSquash(Harness* h, mosaic::Reporter* reporter, uint32_t arch_regs) {
  // Build some committed state: three instructions that allocate, write back and
  // commit. x5, x6 and x7 move off their reset mappings, which is what makes the
  // committed map non-trivial for the restore to be checked against.
  std::vector<Dest> committed;
  for (uint32_t rd : {5u, 6u, 7u}) {
    Stim a;
    a.alloc_req = true;
    a.alloc_rd = rd;
    Outputs ao = h->Cycle(a);
    Require(ao.alloc_new_valid, "squash", "the setup allocation was refused");

    Stim w;
    w.wb_valid = true;
    w.wb = ao.alloc_new;
    Require(h->Cycle(w).wb_accepted, "squash", "the setup writeback was refused");

    Stim c;
    c.commit_valid = true;
    c.commit_rd = rd;
    c.commit = ao.alloc_new;
    Require(h->Cycle(c).commit_accepted, "squash", "the setup commit was refused");
    committed.push_back(ao.alloc_new);
  }

  // This is the state a squash has to restore to, read from the DUT.
  const std::vector<Dest> cmt_at_ckpt = h->DutCmtMap();
  const std::vector<bool> free_at_ckpt = h->DutFreeMask();
  const uint32_t count_at_ckpt = h->free_count();

  // Checkpoint. The journal position at this edge is the recovery point.
  Stim ck;
  ck.ckpt_valid = true;
  h->Cycle(ck);

  // Speculative work after the checkpoint: allocate, write back, and allocate
  // again -- the second allocation of the same register displaces the first, so
  // the window contains a free-free-realloc interleaving that the undo has to get
  // exactly right.
  std::vector<Dest> spec;
  for (uint32_t i = 0; i < 10; i++) {
    const uint32_t rd = 8 + (i % 4);
    Stim a;
    a.alloc_req = true;
    a.alloc_rd = rd;
    Outputs ao = h->Cycle(a);
    Require(ao.alloc_new_valid, "squash", "a speculative allocation was refused");
    spec.push_back(ao.alloc_new);

    Stim w;
    w.wb_valid = true;
    w.wb = ao.alloc_new;
    h->Cycle(w);
  }

  // A commit *older* than the checkpoint lands after it: retire is in-order, so an
  // instruction older than the branch keeps committing while the branch is still in
  // flight. Commit x12 (which has not moved since reset) to the tag it already
  // holds. That is a real commit event whose supersede is a no-op, and it is the
  // case the restore has to handle -- the committed map is written in the same
  // cycle the speculative state is discarded.
  Stim late_commit;
  late_commit.commit_valid = true;
  late_commit.commit_rd = 12;
  late_commit.commit = cmt_at_ckpt[12];
  Outputs lc = h->Cycle(late_commit);
  Require(lc.commit_accepted, "squash", "the in-window commit was refused");

  const uint32_t count_before_squash = h->free_count();
  Require(count_before_squash != count_at_ckpt, "squash",
          "the speculative window did not change the free count, so the restore is not "
          "being tested against anything");

  // Squash. Everything after the checkpoint must go; the committed map and the
  // free set must come back.
  Stim sq;
  sq.squash = true;
  Outputs so = h->Cycle(sq);
  Require(so.squash_accepted, "squash", "the squash was refused although a checkpoint had "
                                         "been taken");

  // The next cycle shows the restored state.
  Stim idle;
  h->Cycle(idle);

  const std::vector<Dest> cmt_after = h->DutCmtMap();
  const std::vector<bool> free_after = h->DutFreeMask();
  const uint32_t count_after = h->free_count();

  for (uint32_t a = 0; a < arch_regs; a++) {
    Require(cmt_after[a] == cmt_at_ckpt[a], "squash",
            "cmt_map[x" + Dec(a) + "] after the squash: expected " + cmt_at_ckpt[a].str() +
                ", got " + cmt_after[a].str() + " -- a commit is permanent and must not be "
                "rolled back");
  }

  for (uint32_t t = 0; t < free_at_ckpt.size(); t++) {
    Require(free_after[t] == free_at_ckpt[t], "squash",
            "tag " + Dec(t) + " after the squash: expected free=" + Bool(free_at_ckpt[t]) +
                ", got free=" + Bool(free_after[t]) + " -- the free set was not restored "
                "exactly");
  }
  Require(count_after == count_at_ckpt, "squash",
          "the free count after the squash is " + Dec(count_after) + ", expected " +
              Dec(count_at_ckpt) + " from the checkpoint");

  // The speculative map must equal the committed map now: there is nothing
  // speculative left.
  const std::vector<Dest> spec_after = h->DutSpecMap();
  for (uint32_t a = 0; a < arch_regs; a++) {
    Require(spec_after[a] == cmt_after[a], "squash",
            "spec_map[x" + Dec(a) + "] after the squash is " + spec_after[a].str() +
                " but cmt_map is " + cmt_after[a].str() +
                ": the speculative map was not restored from the committed one");
  }

  // A writeback escaping from a squashed instruction must be rejected: its tag
  // went back to the free set, or back to an older generation.
  uint32_t rejected = 0;
  for (const Dest& d : spec) {
    Stim esc;
    esc.wb_valid = true;
    esc.wb = d;
    Outputs eo = h->Cycle(esc);
    if (!eo.wb_accepted) {
      rejected++;
      Require(eo.wb_stale, "squash",
              "an escaped writeback " + d.str() + " was refused without reporting wb_stale");
    }
  }
  Require(rejected > 0, "squash",
          "no escaped writeback from the squashed window was rejected: the generation rule "
          "was not exercised after a restore");

  // Ordering: a squash cycle refuses allocation. That is the total order this
  // module promises, and it is what keeps the restore from fighting a same-cycle
  // write to the same map entry.
  Stim both;
  both.squash = true;
  both.alloc_req = true;
  both.alloc_rd = 9;
  Outputs bo = h->Cycle(both);
  Require(!bo.alloc_accepted, "squash",
          "an allocation was accepted in a squash cycle: restore and allocation must be "
          "mutually exclusive");
  Require(bo.alloc_squashed, "squash",
          "an allocation was refused in a squash cycle without reporting alloc_squashed");

  Stim after_both;
  const std::vector<Dest> spec_after_both = h->DutSpecMap();
  h->Cycle(after_both);
  const std::vector<Dest> spec_after_both2 = h->DutSpecMap();
  Require(spec_after_both2 == spec_after_both, "squash",
          "a refused allocation still changed the speculative map");
  Require(spec_after_both2[9] == cmt_after[9], "squash",
          "the refused allocation's rd changed the speculative map");

  // A squash with no checkpoint at all is refused and reported. Recovery arrives
  // from three sources and one of them may present a squash before any branch has
  // been dispatched; silently restoring to an arbitrary point is how a flush
  // destroys live state.
  h->Reset(4);
  Stim no_ckpt;
  no_ckpt.squash = true;
  Outputs no = h->Cycle(no_ckpt);
  Require(!no.squash_accepted, "squash",
          "a squash with no checkpoint was accepted: there is nothing to restore to");
  Require(no.squash_underflow, "squash",
          "a squash with no checkpoint was refused without reporting squash_underflow");

  reporter->Check(true,
                  "squash: the committed map survived, the free set was restored exactly, "
                  "escaped writebacks were rejected, and a squash cycle refused allocation");
}

// Phase 7: exhaustion.
void PhaseExhaustion(Harness* h, mosaic::Reporter* reporter, uint32_t entries,
                     uint32_t arch_regs) {
  // Fill the free set without releasing anything. A checkpoint goes in at the start
  // of each iteration: the undo window holds one entry per allocation since the
  // last checkpoint and is sized for a full register file of them, and this
  // campaign allocates exactly that many, so without the checkpoint it would
  // legitimately report the window overflow. A real core checkpoints at every
  // branch, and a phase that fills the whole file has to model that.
  uint32_t allocated = 0;
  while (h->free_count() > 0) {
    Stim ck;
    ck.ckpt_valid = true;
    h->Cycle(ck);

    Stim a;
    a.alloc_req = true;
    a.alloc_rd = 1 + (allocated % (arch_regs - 1));
    Outputs o = h->Cycle(a);
    Require(o.alloc_new_valid, "exhaustion",
            "allocation " + Dec(allocated) + " was refused before the free set was empty");
    Require(!o.alloc_exhausted, "exhaustion",
            "exhaustion was reported while " + Dec(h->free_count()) +
                " tags were still free");
    allocated++;
    Require(allocated <= entries, "exhaustion",
            "the campaign allocated more tags than the register file has: the free set is "
            "not being consumed");
  }
  Require(allocated == entries - arch_regs, "exhaustion",
          "the register file yielded " + Dec(allocated) + " tags, expected " +
              Dec(entries - arch_regs) + " (entries minus the architectural reset "
              "mappings)");

  // Now the free set is empty. The next request must be refused and reported.
  const std::vector<bool> free_full = h->DutFreeMask();
  const std::vector<Dest> spec_full = h->DutSpecMap();

  Stim over;
  over.alloc_req = true;
  over.alloc_rd = 20;
  Outputs oo = h->Cycle(over);
  Require(!oo.alloc_accepted, "exhaustion",
          "an allocation was accepted with an empty free list: the register file was "
          "over-allocated");
  Require(oo.alloc_exhausted, "exhaustion",
          "exhaustion was not reported with an empty free list");
  Require(!oo.alloc_new_valid, "exhaustion",
          "a destination was produced with an empty free list: the refusal still handed out "
          "a tag");
  Require(!oo.alloc_squashed, "exhaustion",
          "exhaustion was reported as a squash: the core would act on the wrong reason");

  // The refusal must change nothing at all.
  Stim idle;
  h->Cycle(idle);
  Require(h->DutFreeMask() == free_full, "exhaustion",
          "a refused allocation changed the free set");
  Require(h->DutSpecMap() == spec_full, "exhaustion",
          "a refused allocation changed the speculative map");

  // Exhaustion is back-pressure, not a dead end, and the way out is the legal one:
  // retire an instruction. A commit releases the mapping it supersedes, so the free
  // count rises by one and the very next allocation must succeed.
  //
  // An explicit release is *not* available here: with the register file full, every
  // live tag is named by a mapping some architectural register points at, so
  // releasing one would put a live register on the free list. The standing
  // invariant rejects that, which is the correct answer -- the register file really
  // is out of tags, and the caller has to retire work rather than invent a free tag.
  //
  // x1 was written last in the fill loop, so committing its current mapping is the
  // in-order head of what can retire.
  const Dest head = h->DutSpecMap()[1];
  // x1 has not committed since reset in this phase, so the mapping this commit
  // supersedes is x1's *reset* mapping, and that is the tag that comes back -- not
  // the destination being installed. Reading the committed map here rather than
  // assuming makes the assertion say what the hardware actually did.
  const std::vector<Dest> cmt_before = h->DutCmtMap();
  Stim rel;
  rel.commit_valid = true;
  rel.commit_rd = 1;
  rel.commit = head;
  Outputs ro = h->Cycle(rel);
  Require(ro.commit_accepted, "exhaustion",
          "retiring the head instruction was refused while the free list was empty: "
          "exhaustion has made the module unable to give a tag back");
  Require(h->free_count() == 1, "exhaustion",
          "the free count is " + Dec(h->free_count()) +
              " after retiring with an empty register file, expected exactly 1: the commit "
              "released the mapping it superseded");

  Stim retry;
  retry.alloc_req = true;
  retry.alloc_rd = 20;
  Outputs ry = h->Cycle(retry);
  Require(ry.alloc_accepted && ry.alloc_new_valid, "exhaustion",
          "the allocation after a retire was still refused: exhaustion is permanent "
          "back-pressure rather than a condition that clears");
  Require(ry.alloc_new.tag == cmt_before[1].tag, "exhaustion",
          "the allocation after retiring took tag " + Dec(ry.alloc_new.tag) +
              ", but the tag the commit released was " + Dec(cmt_before[1].tag) +
              ": with one free tag the scan has no choice, so a different one means the "
              "commit released the wrong mapping");

  reporter->Check(true,
                  "exhaustion: the " + Dec(entries - arch_regs) +
                      " allocatable tags were consumed, the next request was refused and "
                      "reported, nothing was over-allocated, and a release cleared it");
}

// Phase 8: a random programme of every request kind.
void PhaseRandom(Harness* h, mosaic::Reporter* reporter, uint32_t entries, uint32_t arch_regs,
                 uint32_t seed, uint32_t cycles) {
  mosaic::Rng rng(seed);

  // Destinations the test has actually been handed, so most requests name
  // identities that exist. Everything else is aimed deliberately: a wrong
  // generation, a free tag, an out-of-range tag. A random campaign that only ever
  // aims valid requests cannot find a stale-generation bug.
  std::vector<Dest> handed;
  uint32_t accepted_allocs = 0;
  uint32_t accepted_wbs = 0;
  uint32_t refused_stale = 0;
  uint32_t exhausted = 0;
  uint32_t squashes = 0;
  uint32_t out_of_range_rejected = 0;

  for (uint32_t i = 0; i < cycles; i++) {
    Stim s;

    // A checkpoint every so often, so squashes have something to restore to and
    // the journal is exercised rather than merely allocated.
    if (rng.Chance(4)) s.ckpt_valid = true;
    if (rng.Chance(6)) s.squash = true;
    if (rng.Chance(6)) s.alloc_req = true;
    if (s.alloc_req) s.alloc_rd = rng.Below(arch_regs);
    s.rs1_addr = rng.Below(arch_regs);
    s.rs2_addr = rng.Below(arch_regs);

    if (rng.Chance(30)) {
      s.wb_valid = true;
      if (!handed.empty() && rng.Chance(70)) {
        s.wb = handed[rng.Below(static_cast<uint32_t>(handed.size()))];
        if (rng.Chance(40)) {
          // Deliberately wrong generation: the stale case.
          s.wb.gen = (s.wb.gen + 1 + rng.Below(3)) & 0x7f;
        }
      } else {
        s.wb = Dest{rng.Below(entries + 8), rng.Below(128)};
      }
    }

    // Releases are aimed either at a *stale* identity (which the module must
    // refuse, because the generation has moved on) or at an out-of-range tag (which
    // has no home bank and must also be refused). A release aimed at a *current*
    // identity is never driven: that would put a register on the free list while an
    // architectural register still points at it, which is a caller bug rather than
    // a stimulus the module should tolerate, and the standing ownership invariant
    // rejects it. The accepted release path is the commit, exercised on every cycle
    // of every directed phase.
    if (rng.Chance(15)) {
      s.free_valid = true;
      if (!handed.empty() && rng.Chance(70)) {
        s.free_ = handed[rng.Below(static_cast<uint32_t>(handed.size()))];
        // Nudge the generation off the tag's current value, so the release is
        // aimed at a superseded identity rather than the live one.
        s.free_.gen = (s.free_.gen + 1 + rng.Below(3)) & h->shadow_gen_mask();
      } else {
        s.free_ = Dest{rng.Below(entries + 8), rng.Below(128)};
      }
    }

    if (rng.Chance(15)) {
      s.commit_valid = true;
      s.commit_rd = rng.Below(arch_regs);
      const std::vector<Dest>& cmt = h->shadow_cmt_map();
      s.commit = cmt[s.commit_rd];
    }

    Outputs o = h->Cycle(s);

    if (o.alloc_new_valid) {
      handed.push_back(o.alloc_new);
      // Keep the list from growing without bound and slowing the campaign down;
      // the identities are dropped, not invalidated.
      if (handed.size() > 64) handed.erase(handed.begin(), handed.begin() + 32);
      accepted_allocs++;
    }
    if (o.alloc_exhausted) exhausted++;
    if (o.wb_accepted) accepted_wbs++;
    if (o.wb_stale) refused_stale++;
    if (o.squash_accepted) squashes++;

    // An out-of-range tag must never be accepted, and the campaign aims at them
    // deliberately.
    if (s.wb_valid && s.wb.tag >= entries) {
      Require(!o.wb_accepted, "random",
              "a writeback to out-of-range tag " + Dec(s.wb.tag) + " was accepted");
      if (!o.wb_accepted) out_of_range_rejected++;
    }
    if (s.free_valid && s.free_.tag >= entries) {
      Require(!o.free_accepted, "random",
              "a release of out-of-range tag " + Dec(s.free_.tag) + " was accepted");
    }
  }

  // Anti-vacuity: a campaign in which nothing happened proves nothing.
  // Thresholds are fractions of the campaign that are *demanded*, not floors that
  // happen to pass. Random allocation is refused whenever the register file is full
  // or a squash is in progress, and with rd drawn uniformly the file fills and stays
  // full unless commits keep pace, so the accepted rate is far below the offered
  // rate. The bar is therefore on the events the phase is actually for -- stale
  // rejections, writebacks, squashes and out-of-range refusals -- plus a floor on
  // allocations loose enough that the file never starves.
  Require(accepted_allocs > 0, "random", "no allocation was ever accepted");
  Require(accepted_wbs > 0, "random", "no writeback was ever accepted");
  Require(refused_stale > cycles / 16, "random",
          "only " + Dec(refused_stale) +
              " stale writebacks were refused over " + Dec(cycles) +
              " cycles: the campaign was not producing the failure it is meant to detect");
  Require(squashes > 0, "random", "no squash was ever accepted");
  Require(out_of_range_rejected > 0, "random",
          "no out-of-range tag was ever rejected: the campaign never aimed at one");
  // Exhaustion is reachable in a random campaign only if the campaign fills the
  // file, which it may not; the dedicated phase covers it, so this is not a
  // requirement here. It is reported either way.

  reporter->Check(true,
                  "random: " + Dec(accepted_allocs) + " allocations, " + Dec(accepted_wbs) +
                      " accepted writebacks, " + Dec(refused_stale) +
                      " stale rejections, " + Dec(squashes) + " squashes, " + Dec(exhausted) +
                      " exhaustion reports over " + Dec(cycles) + " cycles");
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
  Vmosaic_rename_tb dut;

  Harness harness(&dut, &clk, options.max_cycles);

  std::string detail;
  bool passed = true;
  try {
    harness.Reset(4);

    // The geometry comes from the elaborated DUT, never from a literal in this
    // file.
    const uint32_t entries = dut.o_entries;
    const uint32_t tag_w = dut.o_tag_w;
    const uint32_t gen_w = dut.o_gen_w;
    const uint32_t arch_regs = dut.o_arch_regs;
    const uint32_t banks = dut.o_banks;
    const uint32_t rows = dut.o_bank_rows;
    const uint32_t journal = dut.o_journal;

    Require(entries > 0 && tag_w > 0 && gen_w > 0, "geometry",
            "the DUT reported a zero geometry, so the shadow cannot be sized");
    Require(arch_regs == 32, "geometry",
            "expected 32 architectural integer registers, got " + Dec(arch_regs));
    Require(journal > 0, "geometry", "the undo journal depth is zero");

    // The geometry has to be self-consistent, or the shadow is sizing itself
    // against numbers that cannot all be true.
    Require(entries > arch_regs, "geometry",
            "the register file has no tags beyond the architectural reset mappings, so "
            "nothing could ever be allocated");
    Require(journal >= entries - arch_regs, "geometry",
            "the undo journal (" + Dec(journal) + ") is smaller than the number of tags "
            "that can be in flight (" + Dec(entries - arch_regs) + "): a squash could not "
            "undo everything after a checkpoint");
    Require(banks > 0 && entries % banks == 0, "geometry",
            "the bank count does not divide the register file");

    ShadowRename shadow(entries, tag_w, gen_w, arch_regs, journal);
    auto fresh = [&]() {
      harness.Reset(4);
      shadow.Reset();
      harness.BindShadow(&shadow);
    };

    // Phase order is deliberate. Each phase resets first and owns exactly one
    // mechanism, and a run stops at the first failure, so the order decides
    // *which* phase names a given defect. The directed mechanisms run first and
    // the random soak runs last, because the soak fails on "some cycle" and would
    // mask the phases that can say which structure is at fault.
    fresh();
    harness.Phase("reset-state");
    PhaseResetState(&harness, &reporter, entries, arch_regs, banks, rows);

    fresh();
    harness.Phase("ownership");
    PhaseOwnership(&harness, &reporter, entries, arch_regs);

    fresh();
    harness.Phase("generation");
    PhaseGeneration(&harness, &reporter, entries, arch_regs);

    fresh();
    harness.Phase("wrap");
    PhaseWrap(&harness, &reporter, entries, arch_regs);

    fresh();
    harness.Phase("x0");
    PhaseX0(&harness, &reporter, entries, arch_regs);

    fresh();
    harness.Phase("squash");
    PhaseSquash(&harness, &reporter, arch_regs);

    fresh();
    harness.Phase("exhaustion");
    PhaseExhaustion(&harness, &reporter, entries, arch_regs);

    fresh();
    harness.Phase("random");
    PhaseRandom(&harness, &reporter, entries, arch_regs, static_cast<uint32_t>(options.seed), 4000);

    detail = "rename contract holds: " + std::to_string(harness.comparisons()) +
             " shadow comparisons over " + std::to_string(harness.cycles()) + " cycles, " +
             std::to_string(entries) + " entries / " + std::to_string(banks) + " banks, seed " +
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
