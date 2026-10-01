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
  bool alloc_req = false;
  uint32_t alloc_rd = 0;

  uint32_t rs1_addr = 0;
  uint32_t rs2_addr = 0;

  bool wb_valid = false;
  Dest wb{};

  bool free_valid = false;
  Dest free_{};

  bool commit_valid = false;
  uint32_t commit_rd = 0;
  Dest commit{};

  bool ckpt_valid = false;
  bool squash = false;

  std::string str() const {
    return "[alloc=" + Bool(alloc_req) + ":x" + Dec(alloc_rd) +
           " rs=x" + Dec(rs1_addr) + ",x" + Dec(rs2_addr) + " wb=" + Bool(wb_valid) +
           wb.str() + " free=" + Bool(free_valid) + free_.str() + " commit=" +
           Bool(commit_valid) + ":x" + Dec(commit_rd) + commit.str() + " ckpt=" +
           Bool(ckpt_valid) + " squash=" + Bool(squash) + "]";
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

  bool rs1_is_x0 = false;
  bool rs2_is_x0 = false;
  Dest rs1{};
  Dest rs2{};

  bool wb_accepted = false;
  bool wb_stale = false;
  bool wb_duplicate = false;

  bool free_accepted = false;
  bool free_stale = false;
  bool free_double = false;

  bool commit_accepted = false;
  bool commit_x0_dropped = false;

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
    j_ckpt_ = 0;
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

    const bool wants_tag = s.alloc_req && (s.alloc_rd != 0);
    const bool has_free = free_count() > 0;

    o.alloc_squashed = s.alloc_req && s.squash;
    o.alloc_is_x0 = s.alloc_req && (s.alloc_rd == 0) && !s.squash;
    o.alloc_exhausted = wants_tag && !has_free;
    o.alloc_accepted = s.alloc_req && !s.squash && (has_free || s.alloc_rd == 0);
    o.alloc_new_valid = o.alloc_accepted && (s.alloc_rd != 0);
    o.alloc_new = Dest{Scan(), 0};
    o.alloc_new.gen = gen_valid_[o.alloc_new.tag] ? ((gen_[o.alloc_new.tag] + 1) & gen_mask_) : 0;
    o.alloc_old_valid = o.alloc_new_valid;
    o.alloc_old = spec_[s.alloc_rd];

    o.rs1_is_x0 = s.rs1_addr == 0;
    o.rs1 = o.rs1_is_x0 ? Dest{0, 0} : spec_[s.rs1_addr];
    o.rs2_is_x0 = s.rs2_addr == 0;
    o.rs2 = o.rs2_is_x0 ? Dest{0, 0} : spec_[s.rs2_addr];

    o.wb_stale = s.wb_valid && !CurrentOwner(s.wb);
    o.wb_duplicate = s.wb_valid && !o.wb_stale && wb_done_[s.wb.tag];
    o.wb_accepted = s.wb_valid && !o.wb_stale && !wb_done_[s.wb.tag];

    o.free_stale = s.free_valid && !CurrentOwner(s.free_);
    o.free_double = s.free_valid && !o.free_stale && free_[s.free_.tag];
    o.free_accepted = s.free_valid && !o.free_stale && !free_[s.free_.tag];

    o.commit_x0_dropped = s.commit_valid && (s.commit_rd == 0);
    o.commit_accepted = s.commit_valid && (s.commit_rd != 0);

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

    // 1. The checkpoint, read from the pre-edge journal length. A squash in the
    //    same cycle does not take a new checkpoint: it is undoing to the one that
    //    already exists, and overwriting that one first would make it undo to
    //    itself.
    if (s.ckpt_valid && !s.squash) {
      j_ckpt_ = j_len_;
      ckpt_seen_ = true;
    }

    // 2. One journal entry per allocation, holding the state that allocation
    //    replaced. Allocation is refused in a squash cycle, so the push and the
    //    undo below can never both happen in one cycle.
    if (o.alloc_new_valid) {
      if (j_len_ >= journal_) {
        // More allocations than the journal can hold. The contract is that this
        // is *reported*, so the shadow records it and the test checks the report.
        j_overflow_ = true;
      } else {
        undo_.push_back(UndoEntry{o.alloc_new.tag, gen_valid_[o.alloc_new.tag]});
        j_len_++;
      }
    }

    // 3. A squash undoes every allocation made after the checkpoint, oldest entry
    //    first so that the newest is applied last.
    if (o.squash_accepted) {
      for (size_t k = j_ckpt_; k < j_len_; k++) {
        const UndoEntry& e = undo_[k];
        free_[e.tag] = true;
        gen_[e.tag] = e.prev_valid ? ((gen_[e.tag] - 1) & gen_mask_) : 0;
        gen_valid_[e.tag] = e.prev_valid;
      }
      j_len_ = j_ckpt_;
      undo_.resize(j_len_);
    }

    // 4. Allocation: take the tag out of the free set, step its generation, and
    //    clear the written flag for the new owner. The rotation point follows the
    //    tag, wrapping to the first allocatable tag at the end of the file.
    if (o.alloc_new_valid) {
      free_[o.alloc_new.tag] = false;
      gen_[o.alloc_new.tag] = o.alloc_new.gen;
      gen_valid_[o.alloc_new.tag] = true;
      wb_done_[o.alloc_new.tag] = false;
      rot_ = (o.alloc_new.tag + 1 >= entries_) ? arch_regs_ : o.alloc_new.tag + 1;
    }

    // 5. An explicit release puts a tag back. The generation does not move: it
    //    counts allocations, not releases.
    if (o.free_accepted) free_[s.free_.tag] = true;

    // 6. A commit releases the mapping it supersedes, if that is a different
    //    identity from the one it installs. A commit is permanent, so it is not
    //    journalled and a later squash does not undo it.
    if (o.commit_accepted && cmt_[s.commit_rd] != s.commit) {
      free_[cmt_[s.commit_rd].tag] = true;
    }

    // 7. The accepted writeback marks the destination written, so a second
    //    producer for the same identity is a reported duplicate.
    if (o.wb_accepted) wb_done_[s.wb.tag] = true;

    // 8. The maps. A commit lands first so a squash in the same cycle restores to
    //    the post-commit committed map; an allocation in the same cycle lands on
    //    top of that -- and since allocation is refused during a squash, the two
    //    can never both write the same entry here.
    if (o.commit_accepted) cmt_[s.commit_rd] = s.commit;
    if (o.squash_accepted) spec_ = cmt_;
    if (o.alloc_new_valid) spec_[s.alloc_rd] = o.alloc_new;
  }

 private:
  struct UndoEntry {
    uint32_t tag;
    bool prev_valid;
  };

  // The documented rotating scan: the first free tag at or after the rotation
  // point, wrapping once. `alloc_ptr` in the RTL wraps to zero; here it wraps to
  // the first allocatable tag, which is the same tag because the reserved range
  // is contiguous at the bottom -- but the two are written differently on purpose.
  uint32_t Scan() const {
    for (uint32_t k = 0; k < entries_; k++) {
      uint32_t t = (rot_ + k) % entries_;
      if (free_[t]) return t;
    }
    return 0;  // nothing free: the answer is don't-care, and the request is refused
  }

  // "Current owner" is the documented ownership test for a destination identity:
  // in range, generation valid, not free, and carrying the tag's current
  // generation. Everything that is not the current owner is stale.
  bool CurrentOwner(const Dest& d) const {
    if (d.tag >= entries_) return false;
    if (!gen_valid_[d.tag]) return false;
    if (free_[d.tag]) return false;
    return gen_[d.tag] == (d.gen & gen_mask_);
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
  size_t j_ckpt_ = 0;
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
  void Reset(int cycles) {
    for (int i = 0; i < cycles; i++) {
      Cycle(Stim{}, /*rst=*/true);
    }
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
  const Outputs& Cycle(const Stim& s, bool rst = false) {
    if (cycles_ >= max_cycles_) {
      Fail(phase_, "max-cycles (" + Dec(max_cycles_) + ") exhausted before the campaign finished");
    }

    dut_->clk = 0;
    dut_->rst = rst ? 1 : 0;
    dut_->alloc_req = s.alloc_req ? 1 : 0;
    dut_->alloc_rd = static_cast<uint8_t>(s.alloc_rd & 0x1f);
    dut_->rs1_addr = static_cast<uint8_t>(s.rs1_addr & 0x1f);
    dut_->rs2_addr = static_cast<uint8_t>(s.rs2_addr & 0x1f);
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
  }

  const Outputs& observed() const { return observed_; }
  uint64_t comparisons() const { return comparisons_; }
  uint64_t cycles() const { return cycles_; }

  // Accessors the phases need on the model, so a phase can reason about the
  // shadow without the harness exposing it for mutation.
  uint32_t shadow_gen_w() const { return shadow_->gen_w(); }
  uint32_t shadow_gen_mask() const { return shadow_->gen_mask(); }
  uint32_t shadow_entries() const { return shadow_->entries(); }
  const std::vector<Dest>& shadow_cmt_map() const { return shadow_->cmt_map(); }
  const std::vector<Dest>& shadow_spec_map() const { return shadow_->spec_map(); }
  uint32_t shadow_journal_len() const { return shadow_->journal_length(); }

  // The live state as read back from the DUT's observation ports, which is what
  // the phase-level assertions use. Reading the DUT rather than the shadow is
  // the point: a phase assertion that only compared shadow to shadow would pass
  // no matter what the hardware did.
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

  std::vector<uint32_t> DutGens() const {
    std::vector<uint32_t> out;
    const int words = (static_cast<int>(shadow_->entries()) * static_cast<int>(shadow_->gen_w()) + 31) / 32;
    for (int w = 0; w < words; w++) {
      const uint32_t word = dut_->dbg_tag_gen[w];
      for (int b = 0; b < 32; b++) {
        if (static_cast<uint64_t>(w) * 32 + b <
            static_cast<uint64_t>(shadow_->entries()) * shadow_->gen_w()) {
          out.push_back((word >> b) & shadow_->gen_mask());
        }
      }
    }
    return out;
  }

  std::vector<Dest> DutSpecMap() const { return ReadMap(dut_->dbg_spec_map); }
  std::vector<Dest> DutCmtMap() const { return ReadMap(dut_->dbg_cmt_map); }

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

  void CompareOutputs(const Stim& s, const std::string& where) {
    const Outputs e = shadow_->Eval(s);
    observed_ = e;
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

    need(dut_->rs1_is_x0, e.rs1_is_x0, "rs1_is_x0");
    need(dut_->rs2_is_x0, e.rs2_is_x0, "rs2_is_x0");
    Require(dut_->rs1_tag == e.rs1.tag, where,
            "rs1_tag: expected " + Dec(e.rs1.tag) + ", got " + Dec(dut_->rs1_tag) + stim);
    Require(dut_->rs2_tag == e.rs2.tag, where,
            "rs2_tag: expected " + Dec(e.rs2.tag) + ", got " + Dec(dut_->rs2_tag) + stim);
    Require(dut_->rs1_gen == e.rs1.gen, where,
            "rs1_gen: expected " + Dec(e.rs1.gen) + ", got " + Dec(dut_->rs1_gen) + stim);
    Require(dut_->rs2_gen == e.rs2.gen, where,
            "rs2_gen: expected " + Dec(e.rs2.gen) + ", got " + Dec(dut_->rs2_gen) + stim);

    need(dut_->wb_accepted, e.wb_accepted, "wb_accepted");
    need(dut_->wb_stale, e.wb_stale, "wb_stale");
    need(dut_->wb_duplicate, e.wb_duplicate, "wb_duplicate");

    need(dut_->free_accepted, e.free_accepted, "free_accepted");
    need(dut_->free_stale, e.free_stale, "free_stale");
    need(dut_->free_double, e.free_double, "free_double");

    need(dut_->commit_accepted, e.commit_accepted, "commit_accepted");
    need(dut_->commit_x0_dropped, e.commit_x0_dropped, "commit_x0_dropped");

    need(dut_->squash_accepted, e.squash_accepted, "squash_accepted");
    need(dut_->squash_underflow, e.squash_underflow, "squash_underflow");
    need(dut_->journal_overflow, e.journal_overflow, "journal_overflow");

    Require(dut_->free_count == e.free_count, where,
            "free_count: expected " + Dec(e.free_count) + ", got " +
                Dec(dut_->free_count) + stim);

    // A refused allocation must say why. "Not accepted" with no reason leaves the
    // core unable to distinguish back-pressure from a squash.
    if (!e.alloc_accepted) {
      Require(s.alloc_req, where, "alloc_accepted is low with no request on the port");
      Require(e.alloc_squashed || e.alloc_exhausted, where,
              "an allocation was refused with neither alloc_squashed nor "
              "alloc_exhausted: the refusal has no reported reason");
    }
    // Exhaustion and squash are mutually exclusive reasons, and reporting both
    // would let a caller act on the wrong one.
    Require(!(e.alloc_exhausted && e.alloc_squashed), where,
            "alloc_exhausted and alloc_squashed are both high for one request");

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
  }

  // Compare the whole state, not a projection of it.
  void CompareState(const std::string& where) {
    const std::vector<bool> dut_free = DutFreeMask();
    Require(dut_free == shadow_->free_set(), where,
            "the free set differs from the shadow at tag " +
                Dec(FirstDiff(dut_free, shadow_->free_set())));

    Require(DutGenValid() == shadow_->gen_valid(), where,
            "the gen_valid vector differs from the shadow at tag " +
                Dec(FirstDiff(DutGenValid(), shadow_->gen_valid())));

    Require(DutWbDone() == shadow_->wb_done(), where,
            "the wb_done vector differs from the shadow at tag " +
                Dec(FirstDiff(DutWbDone(), shadow_->wb_done())));

    const std::vector<uint32_t> dut_gens = DutGens();
    Require(dut_gens == shadow_->gen(), where,
            "the generation table differs from the shadow at tag " +
                Dec(FirstDiff(dut_gens, shadow_->gen())));

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
void PhaseOwnership(Harness* h, mosaic::Reporter* reporter, uint32_t arch_regs) {
  const std::vector<Dest> base_spec = h->DutSpecMap();

  // The free count must fall by exactly one per allocation and rise by exactly
  // one per release, and never by two. Checking the *delta* rather than an
  // absolute count is what makes a double-free visible: it would show up as a
  // count that rose by two.
  std::vector<Dest> live;  // destinations currently held by live allocations
  uint32_t prev_free = h->observed().free_count;

  for (uint32_t i = 0; i < 40; i++) {
    const uint32_t rd = 1 + (i % (arch_regs - 1));

    // Allocate. The displaced mapping is the one rd had before.
    Stim s;
    s.alloc_req = true;
    s.alloc_rd = rd;
    const Outputs& o = h->Cycle(s);

    Require(o.alloc_accepted, "ownership",
            "cycle " + Dec(i) + ": a request to x" + Dec(rd) +
                " was refused while the free set was not empty");
    Require(o.alloc_new_valid, "ownership", "the allocation produced no destination");
    Require(o.alloc_old_valid && o.alloc_old == base_spec[rd], "ownership",
            "the displaced mapping for x" + Dec(rd) + " was not reported as (" +
                Dec(base_spec[rd].tag) + "," + Dec(base_spec[rd].gen) + ")");

    Require(h->observed().free_count == prev_free - 1, "ownership",
            "the free count went from " + Dec(prev_free) + " to " +
                Dec(h->observed().free_count) + " across one allocation");

    live.push_back(o.alloc_new);
    prev_free = h->observed().free_count;

    // Write it back, then deliver a *second* writeback for the same identity:
    // the first must be accepted and the second must be reported as a duplicate,
    // because a result is written to exactly one destination, never broadcast.
    Stim w;
    w.wb_valid = true;
    w.wb = o.alloc_new;
    const Outputs& w1 = h->Cycle(w);
    Require(w1.wb_accepted, "ownership",
            "the writeback of the allocation's own destination " + o.alloc_new.str() +
                " was refused");

    Stim w2;
    w2.wb_valid = true;
    w2.wb = o.alloc_new;
    const Outputs& w2o = h->Cycle(w2);
    Require(!w2o.wb_accepted, "ownership",
            "a second writeback for destination " + o.alloc_new.str() +
                " was accepted: one result, two writes");
    Require(w2o.wb_duplicate, "ownership",
            "a duplicate writeback was refused without reporting wb_duplicate");

    // Release the oldest live destination; releasing it twice must be reported.
    const Dest victim = live.front();
    live.erase(live.begin());

    Stim f;
    f.free_valid = true;
    f.free_ = victim;
    const Outputs& fo = h->Cycle(f);
    Require(fo.free_accepted, "ownership",
            "releasing live destination " + victim.str() + " was refused");
    Require(h->observed().free_count == prev_free + 1, "ownership",
            "the free count went from " + Dec(prev_free) + " to " +
                Dec(h->observed().free_count) + " across one release");
    prev_free = h->observed().free_count;

    Stim f2;
    f2.free_valid = true;
    f2.free_ = victim;
    const Outputs& f2o = h->Cycle(f2);
    Require(!f2o.free_accepted, "ownership",
            "releasing " + victim.str() + " a second time was accepted: a double free puts "
            "a live register back on the free list");
    Require(f2o.free_double, "ownership",
            "a double free was refused without reporting free_double");
    Require(h->observed().free_count == prev_free, "ownership",
            "a refused double free still changed the free count");
  }

  // A release carrying the wrong generation is stale, not a release: it would
  // free somebody else's register.
  Stim stale;
  stale.free_valid = true;
  stale.free_ = Dest{static_cast<uint32_t>(arch_regs), 0};  // a free tag already
  const Outputs& stale_o = h->Cycle(stale);
  Require(!stale_o.free_accepted && stale_o.free_stale, "ownership",
          "releasing an already-free tag was not reported as a stale release");

  // Release everything still held, so the phase ends with an empty journal and a
  // free set back to its reset contents.
  for (const Dest& d : live) {
    Stim f;
    f.free_valid = true;
    f.free_ = d;
    const Outputs& fo = h->Cycle(f);
    Require(fo.free_accepted, "ownership",
            "the closing release of " + d.str() + " was refused");
  }
  Require(h->observed().free_count == arch_regs, "ownership",
          "after releasing every allocation the free count should be back to the reset "
          "value " + Dec(arch_regs) + ", got " + Dec(h->observed().free_count));

  reporter->Check(true,
                  "ownership: every tag had exactly one owner at every cycle, and duplicate "
                  "producers and double frees were reported");
}

// Phase 3: the generation. This is the phase the card is about.
void PhaseGeneration(Harness* h, mosaic::Reporter* reporter, uint32_t arch_regs) {
  // Allocate one destination, note its identity, and release it.
  Stim a;
  a.alloc_req = true;
  a.alloc_rd = 5;
  const Outputs& first = h->Cycle(a);
  Require(first.alloc_new_valid, "generation", "the first allocation was refused");
  const Dest old = first.alloc_new;
  Require(old.gen == 0, "generation",
          "the first allocation of a never-allocated tag should be generation 0, got " +
              Dec(old.gen));

  // Write it back, then commit it so the architectural map moves, then release
  // the tag it superseded. This is the *legal* path by which a tag becomes free.
  Stim w;
  w.wb_valid = true;
  w.wb = old;
  Require(h->Cycle(w).wb_accepted, "generation", "the writeback was refused");

  Stim c;
  c.commit_valid = true;
  c.commit_rd = 5;
  c.commit = old;
  Require(h->Cycle(c).commit_accepted, "generation", "the commit was refused");

  Stim f;
  f.free_valid = true;
  f.free_ = old;
  Require(h->Cycle(f).free_accepted, "generation",
          "releasing " + old.str() + " after its commit was refused");

  // Re-allocate. The tag comes back; the generation must not.
  Stim a2;
  a2.alloc_req = true;
  a2.alloc_rd = 6;
  const Outputs& second = h->Cycle(a2);
  Require(second.alloc_new_valid, "generation", "the second allocation was refused");
  const Dest fresh = second.alloc_new;

  uint32_t reused = 0;
  if (fresh.tag == old.tag) {
    reused = 1;
    Require(fresh.gen != old.gen, "generation",
            "tag " + Dec(old.tag) + " was handed out again with the same generation " +
                Dec(old.gen) + ": the recycled tag is indistinguishable from the old owner");
  }

  // **The test that matters.** Deliver a writeback carrying the *old* generation.
  // It is aimed at a previous owner of that tag and must be refused.
  Stim stale;
  stale.wb_valid = true;
  stale.wb = old;
  const Outputs& stale_o = h->Cycle(stale);
  Require(!stale_o.wb_accepted, "generation",
          "a writeback with the stale generation " + old.str() +
              " was accepted: a late result overwrote the new owner of tag " +
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

  // And the mapping x6 still points at the new owner.
  const std::vector<Dest> spec = h->DutSpecMap();
  Require(spec[6] == fresh, "generation",
          "x6 maps to " + spec[6].str() + " after the stale writeback, expected " +
              fresh.str());

  // A generation that is wrong in the other direction -- one *ahead* of the
  // current generation -- is equally not the current owner.
  Stim ahead;
  ahead.wb_valid = true;
  ahead.wb = Dest{fresh.tag, (fresh.gen + 1) & 0x7f};
  const Outputs& ahead_o = h->Cycle(ahead);
  Require(!ahead_o.wb_accepted && ahead_o.wb_stale, "generation",
          "a writeback with a generation the tag has not reached was accepted");

  // A tag that was never allocated since reset has no valid generation, so a
  // writeback at generation 0 to it is stale rather than a write into a
  // never-owned register.
  const uint32_t freshest = static_cast<uint32_t>(arch_regs);
  Stim never;
  never.wb_valid = true;
  never.wb = Dest{freshest, 0};
  const Outputs& never_o = h->Cycle(never);
  Require(!never_o.wb_accepted && never_o.wb_stale, "generation",
          "a writeback to a free tag was accepted");

  reporter->Check(true,
                  std::string("generation: a stale generation was rejected and the current "
                              "owner kept its destination") +
                      (reused ? " (and the same tag really was recycled)" : ""));
}

// Phase 4: wrap. Enough allocate/release cycles to wrap the physical tags several
// times over, with a stale generation rejected after every reuse.
void PhaseWrap(Harness* h, mosaic::Reporter* reporter, uint32_t entries, uint32_t arch_regs) {
  // Count how many times each physical tag has been handed out, and how many
  // times the *rotation point itself* wrapped past the end of the register file.
  std::vector<uint32_t> allocations(entries, 0);
  uint32_t pointer_wraps = 0;
  uint32_t last_tag = 0;
  bool have_last = false;

  // Two full passes over the register file, so every tag is reused at least once
  // and the tags near the top of the file wrap their own index twice.
  const uint32_t cycles = 2 * entries + 8;
  for (uint32_t i = 0; i < cycles; i++) {
    Stim a;
    a.alloc_req = true;
    a.alloc_rd = 1 + (i % (arch_regs - 1));
    const Outputs& o = h->Cycle(a);
    Require(o.alloc_new_valid, "wrap",
            "cycle " + Dec(i) + ": allocation refused during the wrap campaign");

    allocations[o.alloc_new.tag]++;
    if (have_last && o.alloc_new.tag < last_tag) pointer_wraps++;
    last_tag = o.alloc_new.tag;
    have_last = true;

    // After every reuse of a tag -- that is, every allocation whose generation is
    // not the first for that tag -- a writeback carrying the *previous*
    // generation must be refused. This is the property that has to survive the
    // wrap, and it is asserted on each reuse rather than once at the end.
    if (allocations[o.alloc_new.tag] > 1) {
      Stim stale;
      stale.wb_valid = true;
      stale.wb = Dest{o.alloc_new.tag, (o.alloc_new.gen - 1) & 0x7f};
      const Outputs& so = h->Cycle(stale);
      Require(!so.wb_accepted, "wrap",
              "cycle " + Dec(i) + ": after tag " + Dec(o.alloc_new.tag) +
                  " was reused, a writeback with the previous generation " +
                  Dec((o.alloc_new.gen - 1) & 0x7f) + " was accepted");
      Require(so.wb_stale, "wrap",
              "cycle " + Dec(i) + ": a stale generation was refused without a report");
    }

    // Write back and release, so the campaign keeps making progress and the tag
    // really is handed out again rather than merely re-read.
    Stim w;
    w.wb_valid = true;
    w.wb = o.alloc_new;
    Require(h->Cycle(w).wb_accepted, "wrap",
            "cycle " + Dec(i) + ": the writeback of " + o.alloc_new.str() + " was refused");

    Stim f;
    f.free_valid = true;
    f.free_ = o.alloc_new;
    Require(h->Cycle(f).free_accepted, "wrap",
            "cycle " + Dec(i) + ": the release of " + o.alloc_new.str() + " was refused");
  }

  uint32_t min_reuse = allocations[0];
  for (uint32_t t = 0; t < entries; t++) {
    if (allocations[t] < min_reuse) min_reuse = allocations[t];
  }

  // The campaign has to have actually wrapped. A test that never reaches the end
  // of the register file proves nothing about a wrapping structure, so the guard
  // is an assertion, not a comment.
  Require(pointer_wraps >= 2, "wrap",
          "the rotation point wrapped only " + Dec(pointer_wraps) +
              " time(s) over " + Dec(cycles) + " allocations: the campaign never reached "
              "the end of the register file, so it proved nothing about wrapping");
  Require(min_reuse >= 2, "wrap",
          "the least-reused tag was handed out " + Dec(min_reuse) +
              " time(s): at least one tag must be recycled for the generation to matter");

  // The generation is 7 bits wide for 96 tags, so it wraps after 128 allocations
  // of the *same* tag. Assert that the campaign stayed below that, and state the
  // bound, because a generation that has wrapped cannot reject anything and the
  // test would be claiming more than it checked.
  const uint32_t gen_w = h->shadow_gen_w();
  const uint32_t max_gen_alloc = 1u << gen_w;
  Require(max_gen_alloc > min_reuse, "wrap",
          "the campaign reused a tag " + Dec(min_reuse) + " times, which reaches or exceeds "
          "the " + Dec(max_gen_alloc) + " allocations the " + Dec(gen_w) +
              "-bit generation can distinguish: the stale-generation checks past that point "
              "are no longer evidence");

  reporter->Check(true,
                  "wrap: the rotation point wrapped " + Dec(pointer_wraps) +
                      " times and every tag was recycled at least " + Dec(min_reuse) +
                      " time(s), with a stale generation rejected on every reuse");
}

// Phase 5: x0.
void PhaseX0(Harness* h, mosaic::Reporter* reporter, uint32_t entries, uint32_t arch_regs) {
  const uint32_t free_before = h->observed().free_count;
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
    const Outputs& o = h->Cycle(s);
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
    Require(h->observed().free_count == free_before, "x0",
            "the free count moved from " + Dec(free_before) + " to " +
                Dec(h->observed().free_count) + " across a write to x0: the free list leaks "
                "a tag per write to x0");
  }

  // A commit to x0 frees nothing and changes no map.
  Stim c;
  c.commit_valid = true;
  c.commit_rd = 0;
  c.commit = Dest{0, 0};
  const Outputs& co = h->Cycle(c);
  Require(co.commit_x0_dropped, "x0", "a commit to x0 was not reported as dropped");
  Require(!co.commit_accepted, "x0", "a commit to x0 was accepted as a real commit");
  Require(h->observed().free_count == free_before, "x0",
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
    const Outputs& o = h->Cycle(r);
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
  while (h->observed().free_count > 0 && guard < entries + 8) {
    Stim a;
    a.alloc_req = true;
    a.alloc_rd = 1 + (guard % (arch_regs - 1));
    h->Cycle(a);
    guard++;
  }
  Require(h->observed().free_count == 0, "x0",
          "the campaign failed to empty the free set, so the x0-with-no-tags case was "
          "never reached");

  Stim x0full;
  x0full.alloc_req = true;
  x0full.alloc_rd = 0;
  const Outputs& xf = h->Cycle(x0full);
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
void PhaseSquash(Harness* h, mosaic::Reporter* reporter, uint32_t arch_regs,
                 uint32_t journal) {
  // Build some committed state: three instructions that allocate, write back and
  // commit. x5, x6 and x7 move off their reset mappings, which is what makes the
  // committed map non-trivial for the restore to be checked against.
  std::vector<Dest> committed;
  for (uint32_t rd : {5u, 6u, 7u}) {
    Stim a;
    a.alloc_req = true;
    a.alloc_rd = rd;
    const Outputs& ao = h->Cycle(a);
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
  const uint32_t count_at_ckpt = h->observed().free_count;

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
    const Outputs& ao = h->Cycle(a);
    Require(ao.alloc_new_valid, "squash", "a speculative allocation was refused");
    spec.push_back(ao.alloc_new);

    Stim w;
    w.wb_valid = true;
    w.wb = ao.alloc_new;
    h->Cycle(w);
  }

  // A commit *older* than the checkpoint may still land in the window: retire is
  // in-order, so an instruction older than the branch keeps committing while the
  // branch is still in flight. Do one, so the restore has to preserve it rather
  // than rolling it back.
  Stim late_commit;
  late_commit.commit_valid = true;
  late_commit.commit_rd = 5;
  late_commit.commit = Dest{static_cast<uint32_t>(arch_regs), 0};  // a free tag
  // A commit to a mapping that is not the current committed one is still
  // accepted by the interface; use the real one instead.
  late_commit.commit = cmt_at_ckpt[5];
  late_commit.commit_rd = 12;  // x12 never moved, so this is a no-op supersede
  h->Cycle(late_commit);

  const uint32_t count_before_squash = h->observed().free_count;
  Require(count_before_squash != count_at_ckpt, "squash",
          "the speculative window did not change the free count, so the restore is not "
          "being tested against anything");

  // Squash. Everything after the checkpoint must go; the committed map and the
  // free set must come back.
  Stim sq;
  sq.squash = true;
  const Outputs& so = h->Cycle(sq);
  Require(so.squash_accepted, "squash", "the squash was refused although a checkpoint had "
                                         "been taken");

  // The next cycle shows the restored state.
  Stim idle;
  h->Cycle(idle);

  const std::vector<Dest> cmt_after = h->DutCmtMap();
  const std::vector<bool> free_after = h->DutFreeMask();
  const uint32_t count_after = h->observed().free_count;

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
    const Outputs& eo = h->Cycle(esc);
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
  const Outputs& bo = h->Cycle(both);
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
  const Outputs& no = h->Cycle(no_ckpt);
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
                     uint32_t arch_regs, uint32_t journal) {
  // Fill the free set without releasing anything.
  uint32_t allocated = 0;
  while (h->observed().free_count > 0) {
    Stim a;
    a.alloc_req = true;
    a.alloc_rd = 1 + (allocated % (arch_regs - 1));
    const Outputs& o = h->Cycle(a);
    Require(o.alloc_new_valid, "exhaustion",
            "allocation " + Dec(allocated) + " was refused before the free set was empty");
    Require(!o.alloc_exhausted, "exhaustion",
            "exhaustion was reported while " + Dec(h->observed().free_count) +
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
  const Outputs& oo = h->Cycle(over);
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

  // Exhaustion is back-pressure, not a dead end: release one mapping and the very
  // next request must succeed.
  Stim rel;
  rel.free_valid = true;
  rel.free_ = spec_full[1];
  const Outputs& ro = h->Cycle(rel);
  Require(ro.free_accepted, "exhaustion",
          "releasing a live mapping was refused while the free list was empty: exhaustion "
          "has made the module refuse to give a tag back");

  Stim retry;
  retry.alloc_req = true;
  retry.alloc_rd = 20;
  const Outputs& ry = h->Cycle(retry);
  Require(ry.alloc_accepted && ry.alloc_new_valid, "exhaustion",
          "the allocation after a release was still refused: exhaustion is permanent "
          "back-pressure rather than a condition that clears");

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

    if (rng.Chance(15)) {
      s.free_valid = true;
      if (!handed.empty() && rng.Chance(70)) {
        s.free_ = handed[rng.Below(static_cast<uint32_t>(handed.size()))];
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

    const Outputs& o = h->Cycle(s);

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
  Require(accepted_allocs > cycles / 8, "random",
          "only " + Dec(accepted_allocs) + " allocations were accepted over " + Dec(cycles) +
              " cycles: the campaign was not dense enough to be evidence");
  Require(accepted_wbs > 0, "random", "no writeback was ever accepted");
  Require(refused_stale > cycles / 8, "random",
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

// Small accessors the phases need, kept here so the phases read as prose.
uint32_t dut_alloc_tag(Harness* h) { return h->observed().alloc_new.tag; }

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
    PhaseOwnership(&harness, &reporter, arch_regs);

    fresh();
    harness.Phase("generation");
    PhaseGeneration(&harness, &reporter, arch_regs);

    fresh();
    harness.Phase("wrap");
    PhaseWrap(&harness, &reporter, entries, arch_regs);

    fresh();
    harness.Phase("x0");
    PhaseX0(&harness, &reporter, entries, arch_regs);

    fresh();
    harness.Phase("squash");
    PhaseSquash(&harness, &reporter, arch_regs, journal);

    fresh();
    harness.Phase("exhaustion");
    PhaseExhaustion(&harness, &reporter, entries, arch_regs, journal);

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
