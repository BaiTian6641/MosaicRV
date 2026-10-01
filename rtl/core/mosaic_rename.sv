// ============================================================================
// mosaic_rename -- work packages I-013 and I-014.
//
// Register Allocation Table (committed + speculative), an explicit free list,
// the x0 rule, and the generation that makes a recycled physical tag safe.
// Everything is sized from the generated configuration package: there is no
// second copy of the geometry in this file, and no number here that a profile
// change could contradict.
//
// -------------------------------------------------- what this module owns
//
// Four pieces of state, and each exists for a reason that is not "convenient":
//
//   1. `cmt_map[32]`   the committed (architectural) map: what the ISA says the
//                      architectural registers hold. It only ever changes on an
//                      in-order commit.
//   2. `spec_map[32]`  the speculative map: what an instruction about to issue
//                      must read. It changes on every allocation.
//   3. `free_bits[96]` the free list. A set, not a queue: a physical tag is
//                      free or it is owned, and nothing else.
//   4. `gen[96]`       the allocation generation of each physical tag.
//
// ------------------------------------------------- why a generation exists
//
// A physical tag is a *wrapping* index: `MOSAIC_INT_PRF_TAG_W = $clog2(96) = 7`
// bits names 128 values for 96 tags, and the tag is handed out again as soon as
// its previous owner retires. A tag on its own therefore cannot identify
// in-flight work, and the ABA case is not exotic -- it needs 96 cycles of
// allocation, which is a few hundred instructions of straight-line code.
//
// So every allocation carries a generation, and the generation is part of the
// destination identity the rest of the core passes around:
//
//   * an allocation of tag T produces destination (T, gen(T)+1);
//   * a writeback is accepted only if it carries (T, gen(T)) as it stands *now*;
//   * a late writeback from the previous owner of T carries the old generation
//     and is rejected, so it cannot overwrite the new owner.
//
// `gen` is also what makes a *squash* safe: rolling the generation back to its
// pre-allocation value means a writeback that escapes from a squashed
// instruction is rejected by exactly the same rule as one that was merely late.
// There is no second mechanism, and therefore no second thing to get wrong.
//
// `gen` is not reset. Validity is tracked outside the array in `gen_valid`, one
// reset bit per tag, and a tag that has never been allocated since reset starts
// its life at generation 0. That is the rule rtl/common/mosaic_ram.sv
// documents: reset cost is control state, not DEPTH x WIDTH of storage.
//
// ------------------------------------------------- free-list representation
//
// The free list is a **bitmap**, not a stack, and that is a deliberate choice
// forced by the recovery requirement rather than a taste preference.
//
// A stack (head pointer + array) is O(1) to pop, but its checkpoint is a single
// pointer, and that pointer is *not* a correct snapshot. Consider a tag freed
// while the stack head sits below the checkpoint: the push overwrites a stack
// slot that a later pop will read, and restoring the pointer hands back a
// stack whose contents are no longer the ones that were saved. That is the
// leak the architecture review names as AR-011, in the form where a
// free-list snapshot silently loses a register.
//
// Restoring a set is exact, so this module restores a set. The cost is that
// allocation is a rotating priority scan over 96 bits rather than a pop --
// O(ENTRIES) combinational logic for a stage that retires one allocation per
// cycle. For a first scalar core that is the right trade: a scan that is
// obviously correct and obviously restorable beats a pop that is cheap and
// subtly wrong. A banked or hierarchical free list is a later optimisation and
// must keep the set semantics to stay compatible with this interface.
//
// The restore itself is an **undo journal**: one entry per allocation, holding
// the tag and whether that tag had a valid generation before the allocation. A
// squash walks the journal back to the checkpoint, returning each tag to the
// free set and stepping its generation back down. One bit per entry is the whole
// undo state, because the generation step is exactly invertible; see the undo
// block for why that is true for *any* interleaving inside the window.
//
// Why only allocations are journalled, and not frees:
//
//   * every free in this design is *caused by a commit* (the superseded
//     committed mapping) or is an explicit release of a mapping that is no
//     longer referenced. A commit is permanent: retire is in-order, so a commit
//     that happens after a checkpoint belongs to an instruction *older* than
//     the checkpointing branch, and undoing it would resurrect a mapping the
//     ISA has already committed.
//   * every allocation after a checkpoint belongs to an instruction *younger*
//     than the checkpoint, and a younger instruction cannot have committed --
//     if it had, the checkpointing branch would have committed too and there
//     would be nothing to squash to.
//
// So the journal is exactly the set of events that can be undone, and it is
// bounded by the ROB: at most `MOSAIC_ROB_ENTRIES` instructions can be younger
// than a given checkpoint, so at most that many allocations can be outstanding
// in the journal. `journal_overflow` reports the bound being violated rather
// than silently restoring less than the truth.
//
// -------------------------------------------------- bank decoding, 96 / 4
//
// 96 entries over 4 banks is deliberately not a power of two, so the decode has
// to be stated rather than assumed. The rule:
//
//     bank = tag[BANK_W + ROW_W - 1 : ROW_W]      bank index, 0..BANKS-1
//     row  = tag[ROW_W - 1 : 0]                   row inside the bank
//
// with `ROW_W = $clog2(ENTRIES / BANKS) = $clog2(24) = 5`. `BANK_W + ROW_W` is
// exactly `TAG_W`, so the two fields together are the tag: the decode is a
// partition, not a hash, and every tag has exactly one home bank. Entries per
// bank are `ENTRIES / BANKS = 24`, which the module checks at elaboration
// (a non-divisible split is refused rather than rounded).
//
// Because `ROW_W` is a `$clog2`, a row can name 32 rows of which only 24 exist:
// **tags 96..127 have no home bank.** They are never allocated (the free list
// only ever contains 0..95) and any writeback or free aimed at one is rejected
// as out of range. The rule is "refused, never wrapped", for the reason
// rtl/core/mosaic_predictor.sv states for its own truncation: wrapping an
// out-of-range index onto a valid one manufactures an alias onto somebody
// else's register.
//
// -------------------------------------------------------------- x0
//
// x0 is not a physical register. It reads as zero, a write to it is discarded,
// and -- the part that matters -- **it never consumes a physical tag**. If it
// consumed one, the free list would leak a tag on every write to x0, the leak
// would only become visible as spurious exhaustion dozens of instructions
// later, and the exhaustion would be blamed on something else entirely. x0 is
// handled at both ends: allocation allocates nothing for rd == 0, and a commit
// with rd == 0 frees nothing.
//
// ---------------------------------------------------------- reset
//
// Synchronous, active high. It clears the two maps to the architectural reset
// state (arch reg i mapped to tag i, generation 0), the free set to "every tag
// that is not an architectural reset mapping is free", and the pointer/journal
// control words. It does **not** touch `gen` or the journal storage, for the
// reason given above.
//
// **Tags 0..ARCH_REGS-1 are owned, not free, at reset.** That is the one place
// where the "exactly one owner per physical register" invariant could be broken
// by the reset state itself, so it is worth stating plainly: x5's committed
// mapping is tag 5, therefore tag 5 has an owner, therefore tag 5 must not be on
// the free list. A free list reset to all-ones would hand tag 5 to the first
// instruction that writes any register, giving one physical register two live
// owners and turning a later writeback into a silent corruption of an
// architectural value. The initial mapping of x5 is released the moment the
// first write to x5 *commits*, and the commit path does exactly that -- so the
// tag returns to the pool through the normal path rather than being special.
//
// It also makes the geometry add up: ARCH_REGS owned + (ENTRIES - ARCH_REGS)
// free = 96, and 96 - 32 = 64 = MOSAIC_ROB_ENTRIES, which is precisely the
// number of in-flight instructions the undo journal is sized for. The free
// count at reset is therefore 64, not 96, and a program that has dispatched as
// many instructions as the ROB can hold is genuinely out of tags.
//
// The two maps *are* reset, and that is a deliberate departure from the
// "no full-array reset" rule: a 32-entry file with two combinational read ports
// and one write port is flip-flops, not an inferred RAM, so the rule (which
// exists to stop DEPTH x WIDTH resettable arrays) does not apply to it. Its
// reset state is architecturally defined, so leaving it to power-up contents
// would make the first read of every architectural register undefined.
//
// -------------------------------------------------------- cycle semantics
//
// Everything below is combinational in the cycle it describes and registered at
// the end of that cycle, so a request offered in cycle N has its answer
// (`*_accepted`, `*_stale`, ...) visible in cycle N and its effect visible in
// cycle N+1.
//
//   * Allocation is a **group of up to two macros**, one per lane, in program
//     order; see "two-wide allocation groups" below. A group is accepted whole
//     or refused whole: a half-allocated group would have a mapping with no
//     destination on one lane and a tag nobody owns on the other.
//   * A cycle in which `squash` is high **refuses allocation**. Restore and
//     allocate are mutually exclusive, and the ordering is therefore total
//     rather than "restore wins, mostly". The core is squashing in that cycle
//     anyway; one bubble costs nothing and removes a same-cycle write conflict
//     on the map from the design.
//   * A **commit in a squash cycle is applied before the restore**, so the
//     restored speculative map reflects the commit. The alternative -- restore
//     over the commit -- would resurrect the mapping the commit just replaced.
//   * The allocation scan sees the free set as it was *before* this cycle's
//     edges. A tag freed in cycle N is allocatable from cycle N+1.
//   * The generation of a tag advances only on allocation, never on free, so a
//     tag that cycles free/allocate/free keeps counting up and a stale write
//     from any earlier incarnation is rejected.
//
// -------------------------------------------------- two-wide allocation groups
//
// I-014 widens *allocation* to two macros per cycle, in program order. The
// single-width port set is unchanged and now means "a group of one": `alloc_req`
// is lane 0's valid bit, `alloc2_req` is lane 1's, and a group with `alloc2_req`
// low behaves exactly as it did before this card -- the same scan, the same
// journal, the same answers.
//
// The group is **atomic**. Both lanes are accepted with one combined update of
// the speculative map, the free list and the undo journal, or neither is. A
// partially allocated group -- lane 0 holding a tag while lane 1 is refused --
// would leave the mapping, the free list and the ROB disagreeing about what was
// allocated, which is the failure the card names. The number of tags a group
// needs is therefore the number of its lanes that write a non-x0 destination
// (0, 1 or 2), and the acceptance test is `free_count >= need`, never
// `free_count >= 1`:
//
//   * a group whose two macros both write a real destination and finds one free
//     tag stalls *as a group*, and a later single-width allocation of that same
//     tag succeeds -- the tag was never taken;
//   * a group whose lane 0 writes x0 needs only lane 1's tag, and a group whose
//     two lanes are both x0 needs none at all, so x0 writes never turn tag
//     pressure into a dispatch stall.
//
// The two tags a two-wide group takes are the two tags two consecutive
// single-width allocations would have taken, in the same order: lane 1's scan
// starts one past lane 0's tag and excludes it. That equivalence is what makes
// the group's effect on the free list and on the rotation point describable
// without a second rule.
//
// Lane 1 reads the speculative map as of the start of the cycle with one
// addition: a source that names lane 0's destination resolves to lane 0's *new*
// (tag, generation), and is reported not-ready. That is the same-cycle bypass.
// The map itself is only written at the edge, so the bypass is generated in the
// read path; and because lane 0's producer is in flight rather than in the PRF,
// the bypassed source's readiness bit is 0 by construction -- not by the
// `wb_done` lookup every other source uses. Without the bypass, lane 1 would
// read the tag of the mapping lane 0 is superseding: a register whose producing
// instruction has been renamed away and which nobody will ever write.
//
// The bypass is defined only in a cycle in which lane 0 really allocates. When
// the group is refused there is no new destination to bypass to, and when lane 0
// writes x0 there is none either; in both cases lane 1's sources are exactly the
// map's, and the shadow model and this module have to agree about which of those
// two situations is in force -- hence "the bypass follows the allocation, not the
// request".
//
// WAW inside one group: lane 1 lands on the map after lane 0, so a pair writing
// the same rd leaves lane 1's mapping in the map -- the younger instruction wins,
// as it must -- and the two lanes hold *distinct* tags (lane 1's scan excludes
// lane 0's tag), so no physical register is handed out twice. Lane 1's
// *displaced* mapping is reported as lane 0's new mapping, which is the mapping
// lane 1 actually supersedes; it is not the start-of-cycle mapping, and the
// difference is exactly what makes the two release events come out right. They
// happen through the two commit lanes: lane 0's commit releases the
// start-of-cycle mapping, the `commit2_*` lane releases lane 0's tag, and never
// both at one lane, never the mapping being installed, never the same tag twice.

// ------------------------------------------------------- the recovery point
//
// A checkpoint does two things: it empties the undo window, and it makes the
// state a later squash restores to. The window is emptied *before* the
// checkpoint cycle's own allocations are appended, so the recovery point is the
// state at the **start** of the checkpoint cycle and an allocation made in that
// same cycle is younger than the checkpoint: it is journalled with the rest of
// the window and a squash to that checkpoint undoes it. A branch dispatched as
// lane 1 of a group whose lane 0 allocates in the same cycle is the ordinary
// case, not a corner, so the two have to agree by construction rather than by
// the caller avoiding the combination.
//
// The restore itself is `spec := cmt_map`, and that is only correct where
// `spec == cmt_map` at the checkpoint. This is the module's checkable form of
// "the redirecting branch is the oldest outstanding instruction": an instruction
// that has allocated a tag and not committed has a speculative mapping that
// differs from the committed one for its rd, so equality across all 32
// architectural registers means none is outstanding. At a checkpoint taken with
// such an instruction in flight, the restore would discard *its* mapping while
// its tag stays allocated -- it was allocated before the checkpoint, so it is not
// in the window -- leaving a physical tag neither free nor named by any mapping.
//
// So the module checks it and fails closed:
//
//   * `ckpt_committed` reports the boundary test every cycle, so a caller
//     sampling it in the cycle it takes a checkpoint learns immediately whether
//     that recovery point is usable;
//   * a squash to a checkpoint that was not at a boundary is **refused** and
//     reported as `squash_not_committed`, and nothing is restored. The caller
//     must drain (let the outstanding writes retire) and take a fresh checkpoint,
//     which is the standard full-flush recovery path. Accepting the squash would
//     have converted an unmeetable precondition into silent state corruption --
//     the tags would still be gone from the free list with nothing pointing at
//     them, and the next allocation would hand out a register whose previous
//     value no architectural register can reach.
//   * a squash with no checkpoint at all remains `squash_underflow`: a different
//     refusal, because the caller's fix is different (take one, rather than drain
//     and take one).
//
// The bound this leaves on the caller is stated exactly: **populate `ckpt_valid`
// when the branch is the oldest outstanding instruction** (in ROB terms, at the
// head), which is what makes `spec == cmt` true; the module reports the violation
// rather than relying on the caller remembering. I-018's controller owns the
// richer recovery point (a saved speculative map, which removes the boundary
// requirement altogether); until that is integrated, this module refuses what it
// cannot restore.

// ------------------------------------------------------------- mutants
//
// -DMOSAIC_RENAME_MUTANT_<n> injects exactly one defect to prove the unit test
// can detect it. The shipping build defines none of them; the table with real
// output is in results/reports/I-013-rename.md for the single-width ones and in
// results/reports/I-014-rename2w.md for the two-wide ones.
//
// The two-wide controls (I-014) are:
//
//   NO_BYPASS        lane 1 reads the map as of the start of the cycle and never
//                    sees lane 0's new destination.
//   NONATOMIC_GROUP  lane 0 allocates on the single-lane requirement, so a group
//                    that needs two tags and finds one half-allocates.
//   SAME_TAG_LANE1   lane 1 reuses lane 0's tag instead of the second scan: one
//                    physical register with two owners.
//   WAW_OLD_FROM_MAP lane 1 reports the start-of-cycle mapping as the one it
//                    displaces, so both lanes of a WAW pair claim the same
//                    superseded mapping.
//   WAW_COMMIT2_PRE_MAP
//                    the second commit lane compares against the pre-lane-0
//                    committed map, so a WAW pair releases one old mapping twice
//                    and leaks the other.
//
// X0_ALLOC (from I-013) covers both lanes: a write to x0 on either lane
// allocates a physical tag.
//
// The recovery controls are:
//
//   CKPT_ALLOC_LEAK  an allocation made in the same cycle as a checkpoint is not
//                    journalled (the pre-I-014 rule), so the recovery point is
//                    the end of that cycle while the free set is restored to its
//                    start: one tag neither free nor owned.
//   NO_BOUNDARY_CHECK
//                    a squash to a checkpoint taken with older writers in flight
//                    is accepted: the restore silently loses their mappings and
//                    leaves their tags unreachable.
// ============================================================================

`default_nettype none
`resetall

/* verilator lint_off UNUSEDPARAM */
// The generated header declares one localparam per configuration knob for the
// whole project. This module names the rename/PRF subset; the rest belong to
// other modules and are unused *here* by construction, not by omission.
`include "mosaic_cfg_pkg.svh"
/* verilator lint_on UNUSEDPARAM */

// Widths are declared at file scope because a module's port list cannot see
// declarations inside its own body. They are derived from the generated package
// and nothing else.
localparam int unsigned REN_ENTRIES   = mosaic_cfg_pkg::MOSAIC_INT_PRF_ENTRIES;   // 96
localparam int unsigned REN_TAG_W     = mosaic_cfg_pkg::MOSAIC_INT_PRF_TAG_W;     // 7
localparam int unsigned REN_GEN_W     = mosaic_cfg_pkg::MOSAIC_INT_PRF_TAG_W;     // 7
localparam int unsigned REN_BANKS     = mosaic_cfg_pkg::MOSAIC_PRF_BANKS;         // 4
localparam int unsigned REN_BANK_W    = mosaic_cfg_pkg::MOSAIC_PRF_BANK_W;        // 2
localparam int unsigned REN_ARCH_REGS = mosaic_cfg_pkg::MOSAIC_ARCH_INT_REGS;     // 32
localparam int unsigned REN_ROB       = mosaic_cfg_pkg::MOSAIC_ROB_ENTRIES;       // 64
// The undo window is one entry per allocation since the last checkpoint, bounded
// by the ROB, so its length has to be able to say "full" as well as name every
// index. It is declared here rather than in the module body because `dbg_j_len`
// -- the observation port the unit test compares the window depth through -- is a
// port, and a port's width cannot see a declaration that comes after the port
// list.
localparam int unsigned REN_JLEN_W    = $clog2(REN_ROB + 1);

module mosaic_rename (
    input  logic                                  clk,
    input  logic                                  rst,

    // ---------------------------------------------------------- allocation
    // A group of up to two macros, one per lane, in program order. `alloc_req`
    // is lane 0's valid bit and `alloc2_req` lane 1's; lane 1 without lane 0 is
    // not a group the core can present (program order admits no second macro
    // without a first), and this module refuses it rather than inventing lane 0.
    //
    // The whole group is accepted or the whole group is refused; see "two-wide
    // allocation groups" in the header. `alloc_accepted` is the *group's*
    // answer, and `alloc2_accepted` is the same answer gated on lane 1 being
    // present, so a caller that checks one lane's accept can always see the
    // other's.
    //
    // `alloc_rd == 0` is a write to x0 and is accepted without allocating
    // anything -- on either lane, and independently: a group of two x0 writes
    // needs no tags at all.
    input  logic                                  alloc_req,
    input  logic [4:0]                            alloc_rd,
    output logic                                  alloc_accepted,
    output logic                                  alloc_exhausted,  // refused: the group's tags are not there
    output logic                                  alloc_squashed,   // refused: squash owns the cycle
    output logic                                  alloc_is_x0,      // accepted, and it was a write to x0
    output logic                                  alloc_new_valid,  // a physical tag was allocated
    output logic [REN_TAG_W-1:0]                  alloc_new_tag,
    output logic [REN_GEN_W-1:0]                  alloc_new_gen,
    output logic                                  alloc_old_valid,  // the mapping rd had before
    output logic [REN_TAG_W-1:0]                  alloc_old_tag,
    output logic [REN_GEN_W-1:0]                  alloc_old_gen,

    // Lane 1 of the same group. Its `old` mapping is lane 0's *new* mapping when
    // both lanes write the same rd, because that is the mapping lane 1 really
    // supersedes; the release of the two happens through the two commit lanes,
    // one each.
    input  logic                                  alloc2_req,
    input  logic [4:0]                            alloc2_rd,
    output logic                                  alloc2_accepted,
    output logic                                  alloc2_exhausted,  // refused: the group's tags are not there
    output logic                                  alloc2_squashed,   // refused: squash owns the cycle
    output logic                                  alloc2_is_x0,
    output logic                                  alloc2_new_valid,
    output logic [REN_TAG_W-1:0]                  alloc2_new_tag,
    output logic [REN_GEN_W-1:0]                  alloc2_new_gen,
    output logic                                  alloc2_old_valid,
    output logic [REN_TAG_W-1:0]                  alloc2_old_tag,
    output logic [REN_GEN_W-1:0]                  alloc2_old_gen,

    // ------------------------------------------------------- source reads
    // Combinational, zero latency: the address presented in cycle N yields the
    // tag in cycle N. Four read ports, because a two-wide group renames two
    // instructions and each reads two sources; they are numbered as one flat
    // list in (macro, source) order -- rs1/rs2 belong to lane 0, rs3/rs4 to
    // lane 1.
    //
    // `rs*_ready` is this module's readiness view: the source's producer has
    // written its result back, so the value is in the PRF. A source that is x0
    // is ready (there is nothing to read) and a source resolved by the
    // same-cycle bypass is *not* -- lane 0's producer is in flight by
    // construction, which is exactly what `rs*_bypass` reports.
    input  logic [4:0]                            rs1_addr,
    input  logic [4:0]                            rs2_addr,
    output logic                                  rs1_is_x0,
    output logic                                  rs2_is_x0,
    output logic                                  rs1_ready,
    output logic                                  rs2_ready,
    output logic [REN_TAG_W-1:0]                  rs1_tag,
    output logic [REN_TAG_W-1:0]                  rs2_tag,
    output logic [REN_GEN_W-1:0]                  rs1_gen,
    output logic [REN_GEN_W-1:0]                  rs2_gen,
    input  logic [4:0]                            rs3_addr,
    input  logic [4:0]                            rs4_addr,
    output logic                                  rs3_is_x0,
    output logic                                  rs4_is_x0,
    output logic                                  rs3_ready,
    output logic                                  rs4_ready,
    output logic                                  rs3_bypass,   // resolved from lane 0's new destination
    output logic                                  rs4_bypass,
    output logic [REN_TAG_W-1:0]                  rs3_tag,
    output logic [REN_TAG_W-1:0]                  rs4_tag,
    output logic [REN_GEN_W-1:0]                  rs3_gen,
    output logic [REN_GEN_W-1:0]                  rs4_gen,

    // ------------------------------------------------------- writeback
    // The result of an instruction lands on the destination identity its
    // allocation handed out: (tag, generation). A writeback whose generation
    // is not the tag's current generation is aimed at a previous owner and is
    // rejected. A second writeback for the same identity is a duplicate
    // producer and is also rejected: a result is written to exactly one
    // destination, never broadcast.
    input  logic                                  wb_valid,
    input  logic [REN_TAG_W-1:0]                  wb_tag,
    input  logic [REN_GEN_W-1:0]                  wb_gen,
    output logic                                  wb_accepted,
    output logic                                  wb_stale,      // not the tag's current owner
    output logic                                  wb_duplicate,  // already written this generation

    // -------------------------------------------------------------- free
    // Release a live mapping. Reported when it is refused: a stale generation,
    // or a tag that is already free. A double free is a rename-side bug that
    // would otherwise show up much later as a spurious exhaustion.
    input  logic                                  free_valid,
    input  logic [REN_TAG_W-1:0]                  free_tag,
    input  logic [REN_GEN_W-1:0]                  free_gen,
    output logic                                  free_accepted,
    output logic                                  free_stale,
    output logic                                  free_double,

    // ------------------------------------------------------------ commit
    // In-order retire of an architectural register write. Updates the committed
    // map and releases the mapping that write superseded.
    //
    // `commit2_*` is the **second commit lane**, added by I-017: retire is
    // MOSAIC_RETIRE_WIDTH wide, so a cycle can retire two instructions that both
    // write an architectural register, and a one-commit port cannot install
    // both mappings. The lane is applied strictly after lane 0, which is what
    // makes two commits to the same rd in one cycle come out right: the younger
    // mapping wins and the older tag is released.
    //
    // Both lanes default to inactive, so a caller that retires one-wide is
    // unaffected: `commit2_valid` low makes every `commit2_*` output an
    // acceptance refusal and leaves the maps and the free set exactly as they
    // were.
    input  logic                                  commit_valid,
    input  logic [4:0]                            commit_rd,
    input  logic [REN_TAG_W-1:0]                  commit_tag,
    input  logic [REN_GEN_W-1:0]                  commit_gen,
    output logic                                  commit_accepted,
    output logic                                  commit_x0_dropped,  // a write to x0 retires
    input  logic                                  commit2_valid,
    input  logic [4:0]                            commit2_rd,
    input  logic [REN_TAG_W-1:0]                  commit2_tag,
    input  logic [REN_GEN_W-1:0]                  commit2_gen,
    output logic                                  commit2_accepted,
    output logic                                  commit2_x0_dropped,

    // ---------------------------------------------------------- recovery
    // `ckpt_valid` marks the recovery point. A checkpoint empties the undo
    // window, so the recovery point it establishes is the state at the **start**
    // of the checkpoint cycle: an allocation made in that same cycle is younger
    // than the checkpoint and is journalled with the rest of the window.
    //
    // `squash` rolls the free list back to it and restores the speculative map
    // from the committed map. Two refusals, reported separately because they need
    // different responses: a squash with no checkpoint has nothing to restore to
    // (`squash_underflow`, the caller takes a checkpoint first), and a squash to a
    // checkpoint that was *not* at a committed boundary cannot be restored exactly
    // (`squash_not_committed`, the caller must take a fresh recovery point after
    // draining). Refusing the second is the difference between reporting a
    // precondition and silently corrupting state, so it is enforced here rather
    // than left to a comment.
    //
    // `ckpt_committed` reports whether the *current* state is a committed
    // boundary, i.e. whether a checkpoint taken now would be a recovery point a
    // squash can restore. It is meaningful every cycle, and it is the signal a
    // caller should sample in the cycle it asserts `ckpt_valid`.
    input  logic                                  ckpt_valid,
    input  logic                                  squash,
    output logic                                  squash_accepted,
    output logic                                  squash_underflow,      // squash with no checkpoint
    output logic                                  squash_not_committed,  // squash to a non-boundary checkpoint
    output logic                                  ckpt_committed,        // a checkpoint now would be restorable
    output logic                                  journal_overflow,      // undo bound exceeded

    // --------------------------------------------------------- occupancy
    // Free tags currently available. Defined as the population count of the
    // free set itself, so it cannot disagree with the set it reports on.
    output logic [REN_TAG_W:0]                    free_count,

    // ------------------------------------------------ verification output
    // These exist so the unit test can compare the *whole* state every cycle
    // instead of a projection of it. They are read-only observation; nothing
    // in the design consumes them.
    output logic [REN_ENTRIES-1:0]                dbg_free_mask,
    output logic [REN_ENTRIES-1:0]                dbg_gen_valid,
    output logic [REN_ENTRIES-1:0]                dbg_wb_done,
    output logic [REN_ENTRIES*REN_GEN_W-1:0]      dbg_tag_gen,
    output logic [REN_ARCH_REGS*(REN_TAG_W+REN_GEN_W)-1:0] dbg_spec_map,
    output logic [REN_ARCH_REGS*(REN_TAG_W+REN_GEN_W)-1:0] dbg_cmt_map,
    // The undo window depth. Without it, "the group was journalled as a group"
    // could only be inferred from the free set one squash later; the depth is what
    // says a two-wide group pushed exactly two entries, in the cycle it did so.
    output logic [REN_JLEN_W-1:0]                 dbg_j_len
);

  // ---------------------------------------------------------- geometry rules
  // Derived widths. A $clog2 of 1 is zero bits, which would produce a
  // zero-width part select, so each one has the same guard mosaic_fifo's PtrW
  // has.
  localparam int unsigned REN_ROW_W     = (REN_ENTRIES / REN_BANKS) <= 1
                                           ? 1 : $clog2(REN_ENTRIES / REN_BANKS);
  localparam int unsigned REN_ARCH_W    = (REN_ARCH_REGS <= 1) ? 1 : $clog2(REN_ARCH_REGS);
  localparam int unsigned REN_MAP_W     = REN_TAG_W + REN_GEN_W;
  localparam int unsigned REN_SCAN_W    = REN_TAG_W + 1;  // address into the doubled mask
  localparam int unsigned REN_FCNT_W    = REN_TAG_W + 1;  // must represent ENTRIES itself
  localparam int unsigned REN_BANK_ROWS = REN_ENTRIES / REN_BANKS;

  // A false branch of a generate is never elaborated, so naming a module that
  // does not exist is an elaboration-time error rather than a runtime one.
  // These are the geometry preconditions the header promises; each violation
  // means the bank split, the tag width or the address width is not what this
  // module's decode assumes, and it must fail at build time rather than
  // quietly mis-decode.
  generate
    if ((REN_ENTRIES % REN_BANKS) != 0) begin : g_bad_bank_split
      mosaic_rename_contract_violation u_bank_split();
    end
    if ((1 << REN_TAG_W) < REN_ENTRIES) begin : g_bad_tag_width
      mosaic_rename_contract_violation u_tag_width();
    end
    if ((1 << REN_BANK_W) < REN_BANKS) begin : g_bad_bank_width
      mosaic_rename_contract_violation u_bank_width();
    end
    if ((1 << REN_ROW_W) < REN_BANK_ROWS) begin : g_bad_row_width
      mosaic_rename_contract_violation u_row_width();
    end
    // bank + row must be exactly the tag, or the decode stops being a
    // partition and two tags could name one bank row.
    if ((REN_BANK_W + REN_ROW_W) != REN_TAG_W) begin : g_bad_bank_decode
      mosaic_rename_contract_violation u_bank_decode();
    end
    // The architectural register address is 5 bits (RISC-V x0..x31); if the
    // profile had fewer registers the address could name one that does not
    // exist, which the port list cannot express.
    if (REN_ARCH_W != 5) begin : g_bad_arch_width
      mosaic_rename_contract_violation u_arch_width();
    end
    if ((1 << REN_FCNT_W) <= REN_ENTRIES) begin : g_bad_fcount_width
      mosaic_rename_contract_violation u_fcount_width();
    end
  endgenerate

  // ------------------------------------------------------------------ state
  // Reset state. The maps are flip-flops (two combinational reads, one write),
  // so they are reset to the architectural reset state: arch reg i -> tag i at
  // generation 0.
  logic [REN_TAG_W-1:0] spec_map [REN_ARCH_REGS];
  logic [REN_GEN_W-1:0] spec_gen [REN_ARCH_REGS];
  logic [REN_TAG_W-1:0] cmt_map  [REN_ARCH_REGS];
  logic [REN_GEN_W-1:0] cmt_gen  [REN_ARCH_REGS];

  // Free set: one reset bit per entry. Validity of the rest of the per-tag
  // state lives outside the arrays, in gen_valid and wb_done.
  logic [REN_ENTRIES-1:0] free_bits;
  logic [REN_ENTRIES-1:0] gen_valid;
  logic [REN_ENTRIES-1:0] wb_done;

  // Data arrays: never reset. See "why a generation exists" in the header.
  logic [REN_GEN_W-1:0]  gen  [REN_ENTRIES];
  logic [REN_TAG_W-1:0]  j_tag [REN_ROB];
  logic                  j_prev_valid [REN_ROB];

  // Control state: the rotation point of the allocation scan, the length of the
  // undo window, whether any checkpoint has ever been taken, and the report that
  // the window overflowed. There is no checkpoint *position* to save: a
  // checkpoint empties the window rather than marking a point inside it, which is
  // what makes the window's bound equal to "allocations since the last
  // checkpoint" -- the quantity the ROB size justifies.
  logic [REN_TAG_W-1:0]  alloc_ptr;
  logic [REN_JLEN_W-1:0] j_len;
  logic                  ckpt_seen;
  logic                  j_overflow;
  // Whether the checkpoint that is the current recovery point was taken at a
  // committed boundary. See `ckpt_committed` below for what that means and why
  // the module refuses a squash to a checkpoint that was not.
  logic                  ckpt_at_boundary;

  // ------------------------------------------------------------ small helpers

  // The reset value of the free set: every tag except the ARCH_REGS
  // architectural reset mappings, which are owned by those mappings rather than
  // free. Expressed as a function so the reserved range is written once and
  // cannot drift from the map reset it has to agree with.
  function automatic logic [REN_ENTRIES-1:0] init_owned_mask();
    logic [REN_ENTRIES-1:0] mask;
    mask = {REN_ENTRIES{1'b1}};
    for (int unsigned a = 0; a < REN_ARCH_REGS; a++) begin
      mask[a] = 1'b0;
    end
    return mask;
  endfunction

  // Population count of the free set. This *is* the occupancy report, so the two
  // cannot drift apart.
  function automatic logic [REN_FCNT_W-1:0] count_free(input logic [REN_ENTRIES-1:0] bits);
    logic [REN_FCNT_W-1:0] acc;
    acc = {REN_FCNT_W{1'b0}};
    for (int unsigned b = 0; b < REN_ENTRIES; b++) begin
      acc = acc + REN_FCNT_W'(bits[b]);
    end
    return acc;
  endfunction

  // Next-state images of the four map files. Declared here rather than beside
  // the block that fills them, because the second commit lane reads `cmt_map_q`
  // from the free-set block further down: lane 1 has to know what lane 0
  // installed. Verilator resolves a forward reference and slang does not, so a
  // declaration in the natural place is accepted by one tool and rejected by
  // the other -- which is exactly the class of difference this project has been
  // bitten by before.
  logic [REN_TAG_W-1:0] spec_map_q [REN_ARCH_REGS];
  logic [REN_GEN_W-1:0] spec_gen_q [REN_ARCH_REGS];
  logic [REN_TAG_W-1:0] cmt_map_q  [REN_ARCH_REGS];
  logic [REN_GEN_W-1:0] cmt_gen_q  [REN_ARCH_REGS];


  // ------------------------------------------------------- allocation scan
  // A rotating priority scan over the free set: the first free tag at or after
  // the rotation point, wrapping once. Rotating is what makes a tag wrap its own
  // index reachable in a bounded number of cycles, so the wrap is testable rather
  // than something a campaign has to run for a million cycles to find.
  //
  // The scan is one function called twice rather than two copies of the loop. The
  // second call belongs to lane 1 and must be the *same* scan: two copies that
  // could drift would let lane 1 take a tag lane 0 is holding, and that duplicate
  // would surface as a physical register with two owners rather than as a failure
  // here. `mask` is doubled inside the function so that the wrapped candidate at
  // offset ENTRIES+k reads the same bit as the unwrapped offset k -- which is what
  // makes the whole wrap one conditional subtract instead of a second loop.
  function automatic logic [REN_TAG_W-1:0] scan_free(input logic [REN_ENTRIES-1:0] mask,
                                                     input logic [REN_TAG_W-1:0] ptr);
    logic [2*REN_ENTRIES-1:0] dup;
    logic                     seen;
    dup       = {mask, mask};
    seen      = 1'b0;
    scan_free = {REN_TAG_W{1'b0}};
    for (int unsigned i = 0; i < REN_ENTRIES; i++) begin
      // The candidate at offset i is (rotation point + i) mod ENTRIES. The sum
      // is at most 2*ENTRIES-2, so one conditional subtract is the whole
      // modulus and no general division is elaborated. Both operands are
      // widened to the scan width first, so neither the add nor the compare can
      // silently truncate the tag.
      logic [REN_SCAN_W-1:0] off;
      logic [REN_TAG_W-1:0]  idx;
      off = REN_SCAN_W'(ptr) + REN_SCAN_W'(i);
      idx = REN_TAG_W'((off >= REN_SCAN_W'(REN_ENTRIES)) ? (off - REN_SCAN_W'(REN_ENTRIES)) : off);
      if (!seen && dup[off]) begin
        seen      = 1'b1;
        scan_free = REN_TAG_W'(idx);
      end
    end
  endfunction

  // Whether a scan matched is deliberately *not* an output: the group's
  // acceptance test is the free *count* against the tags the group needs, and a
  // "did the scan find anything" bit would be a second, weaker statement of the
  // same fact -- one that a two-tag group could satisfy with one free tag.
  logic [REN_TAG_W-1:0] scan_tag;      // lane 0's tag
  logic [REN_TAG_W-1:0] scan_tag2;     // lane 1's tag

  // Whether lane 0's request wants a tag at all. This is a pure function of the
  // request -- not of the group's acceptance -- and lane 1's scan is steered by
  // it, because "which tag lane 1 takes" must have one answer per cycle whether
  // or not the group is finally accepted.
  logic                   lane0_wants_tag;
  logic [REN_ENTRIES-1:0] lane1_scan_mask;
  logic [REN_TAG_W-1:0]   lane1_scan_ptr;

  assign lane0_wants_tag = alloc_req && (alloc_rd != 5'd0);

  // Lane 1's scan is the one two consecutive single-width allocations would
  // produce: it starts one past lane 0's tag and cannot see that tag. When lane 0
  // takes no tag (an x0 lane, or a group of one) lane 1 *is* the first
  // allocation, so it takes lane 0's scan unchanged.
  //
  // The mask clears lane 0's tag from the full 96-bit vector, so a tag of 95
  // lands in bit 95 and nothing is truncated. The pointer wraps to zero at the
  // end of the file exactly as the post-allocation rotation does.
  assign lane1_scan_mask = lane0_wants_tag
                           ? (free_bits & ~(REN_ENTRIES'(1) << scan_tag))
                           : free_bits;
  assign lane1_scan_ptr  = lane0_wants_tag
                           ? ((scan_tag == REN_TAG_W'(REN_ENTRIES - 1))
                                ? {REN_TAG_W{1'b0}} : (scan_tag + REN_TAG_W'(1)))
                           : alloc_ptr;

  always_comb begin
    scan_tag  = scan_free(free_bits,       alloc_ptr);
    scan_tag2 = scan_free(lane1_scan_mask, lane1_scan_ptr);
  end

  // --------------------------------------------------- group accept / refusal
  // Each refusal has its own report. "Refused" with no reason would leave the
  // core unable to tell back-pressure (retry later) from a squash (do not retry)
  // from an x0 write (nothing to retry at all).
  logic       alloc2_wants_tag;
  logic [1:0] group_need;
  logic       group_enough;

  assign alloc2_wants_tag = alloc2_req && (alloc2_rd != 5'd0);

  // How many tags the group needs is how many of its lanes write a real
  // destination: 0, 1 or 2. The two-bit sum is deliberate -- a one-bit sum would
  // report a two-tag group as needing none, which is the "always stall on 2"
  // defect's mirror image.
  assign group_need   = {1'b0, lane0_wants_tag} + {1'b0, alloc2_wants_tag};
  // The free count is compared as a number, not as "is anything free": a group
  // that needs two tags is refused when only one is free, and refusing it here --
  // before any tag is taken -- is what makes the group atomic.
  assign group_enough = (REN_FCNT_W'(free_count) >= REN_FCNT_W'(group_need));

  assign alloc_squashed  = alloc_req && squash;
  assign alloc2_squashed = alloc2_req && squash;
  assign alloc_is_x0     = alloc_req && (alloc_rd == 5'd0) && !squash;
  assign alloc2_is_x0    = alloc2_req && (alloc2_rd == 5'd0) && !squash;

`ifdef MOSAIC_RENAME_MUTANT_NO_EXHAUST_CHECK
  // NEGATIVE CONTROL 4 (I-013): exhaustion is not detected. A group is taken and
  // tags are produced even when the free set is empty -- which means the tags
  // belong to somebody else, and nothing in the core can tell. Both lanes are
  // accepted, so the defect is visible whichever lane wants the tag.
  assign alloc_exhausted  = 1'b0;
  assign alloc2_exhausted = 1'b0;
  assign alloc_accepted   = alloc_req && !squash;
  assign alloc2_accepted  = alloc_req && alloc2_req && !squash;
`elsif MOSAIC_RENAME_MUTANT_NONATOMIC_GROUP
  // NEGATIVE CONTROL 7 (I-014): the group is not atomic. Each lane is accepted on
  // its *own* requirement, so a group that needs two tags and finds one allocates
  // lane 0 and stalls lane 1 -- the half-allocated group the card forbids. Lane 0
  // is left holding a tag the ROB never records as allocated, and lane 1's
  // instruction is refused after its predecessor's tag has been consumed.
  assign alloc_exhausted  = alloc_req && !squash && lane0_wants_tag && (free_count == 0);
  assign alloc2_exhausted = alloc_req && alloc2_req && !squash && alloc2_wants_tag &&
                            (free_count == 0);
  assign alloc_accepted   = alloc_req && !squash &&
                            ((free_count != 0) || (alloc_rd == 5'd0));
  assign alloc2_accepted  = alloc_req && alloc2_req && !squash &&
                            ((free_count != 0) || (alloc2_rd == 5'd0));
`else
  // The two refusal reasons are mutually exclusive, and they have to be. A squash
  // is going to discard the group regardless of whether tags were available, so
  // reporting exhaustion as well would leave the caller unable to tell "retry
  // after the squash" from "retry when a tag frees" -- and the two need opposite
  // behaviour from everything upstream. A squash wins.
  //
  // The acceptance test is the *group's* requirement, never lane 0's alone: a
  // group of two real destinations with one free tag is refused whole, and the
  // tag it did not take is still free for the next cycle's single-width
  // allocation. Both lanes report the same refusal, because it is one decision.
  assign alloc_exhausted  = alloc_req && !squash && (group_need != 2'd0) && !group_enough;
  assign alloc2_exhausted = alloc_req && alloc2_req && !squash && (group_need != 2'd0) &&
                            !group_enough;
  assign alloc_accepted   = alloc_req && !squash && group_enough;
  assign alloc2_accepted  = alloc_req && alloc2_req && !squash && group_enough;
`endif

`ifdef MOSAIC_RENAME_MUTANT_X0_ALLOC
  // NEGATIVE CONTROL 2: a write to x0 allocates a physical tag like any other
  // destination, on either lane. The free count drops by one per write to x0, and
  // the leak only becomes visible as spurious exhaustion dozens of instructions
  // later, where nothing points back at the x0 writes that caused it.
  assign alloc_new_valid  = alloc_accepted;
  assign alloc2_new_valid = alloc2_accepted;
`else
  assign alloc_new_valid  = alloc_accepted  && (alloc_rd  != 5'd0);
  assign alloc2_new_valid = alloc2_accepted && (alloc2_rd != 5'd0);
`endif

  assign alloc_new_tag   = scan_tag;
  assign alloc_new_gen   = gen_valid[scan_tag] ? (gen[scan_tag] + REN_GEN_W'(1))
                                               : {REN_GEN_W{1'b0}};

`ifdef MOSAIC_RENAME_MUTANT_SAME_TAG_LANE1
  // NEGATIVE CONTROL 8 (I-014): lane 1 reuses lane 0's tag instead of taking the
  // second scan's. One physical register is handed out twice inside one group,
  // and the generation lane 1 stamps on it makes lane 0's writeback stale, so the
  // first macro's result can never be written.
  assign alloc2_new_tag  = scan_tag;
`else
  assign alloc2_new_tag  = scan_tag2;
`endif
  assign alloc2_new_gen  = gen_valid[alloc2_new_tag] ? (gen[alloc2_new_tag] + REN_GEN_W'(1))
                                                     : {REN_GEN_W{1'b0}};

  // The displaced mapping: what rd pointed at before this allocation. It is
  // owned by the instruction that allocated it and is released when *that*
  // instruction commits -- never here, because it is still being read by
  // instructions younger than the one being renamed.
  assign alloc_old_valid = alloc_new_valid;
  assign alloc_old_tag   = spec_map[alloc_rd];
  assign alloc_old_gen   = spec_gen[alloc_rd];

  // Lane 1's displaced mapping. For a WAW pair -- both lanes of the group writing
  // the same rd -- it is lane 0's *new* destination, because that is the mapping
  // lane 1 really supersedes: lane 0's commit releases the start-of-cycle
  // mapping and lane 1's releases lane 0's tag. Reporting the start-of-cycle
  // mapping here instead would leave both macros claiming one superseded tag and
  // the tag lane 0 allocated never released, which is the WAW control below.
  //
  // The test is `alloc_new_valid`, so a pair whose lane 0 writes x0 has lane 1
  // displacing the map's mapping as usual -- there is no new destination to
  // supersede when lane 0 allocates nothing.
  assign alloc2_old_valid = alloc2_new_valid;
`ifdef MOSAIC_RENAME_MUTANT_WAW_OLD_FROM_MAP
  // NEGATIVE CONTROL 9 (I-014): lane 1 displaces the start-of-cycle mapping even
  // in a WAW pair, so both lanes name the same superseded tag.
  assign alloc2_old_tag   = spec_map[alloc2_rd];
  assign alloc2_old_gen   = spec_gen[alloc2_rd];
`else
  assign alloc2_old_tag   = (alloc_new_valid && (alloc2_rd == alloc_rd))
                            ? alloc_new_tag : spec_map[alloc2_rd];
  assign alloc2_old_gen   = (alloc_new_valid && (alloc2_rd == alloc_rd))
                            ? alloc_new_gen : spec_gen[alloc2_rd];
`endif

  // --------------------------------------------------------- source read ports
  // Combinational, zero latency: the address presented in cycle N yields the tag
  // in cycle N. Lane 1's two ports read the same map plus the same-cycle bypass
  // described in the header: a source naming lane 0's destination resolves to
  // lane 0's new (tag, generation) and is reported not-ready, because that
  // producer is in flight rather than in the PRF.
  //
  // The bypass follows the *allocation*, not the request: it is armed only when
  // lane 0 really allocated a tag this cycle. In a refused group there is no new
  // destination to bypass to, and when lane 0 writes x0 there is none either, so
  // lane 1 reads the map -- which is what the shadow model does as well, and the
  // two have to agree cycle by cycle.
  logic rs3_hits_lane0;
  logic rs4_hits_lane0;

  // `alloc_new_valid` already implies a non-zero alloc_rd, so a source address of
  // x0 can never match: the bypass cannot fire for a register with no physical
  // mapping.
`ifdef MOSAIC_RENAME_MUTANT_NO_BYPASS
  // NEGATIVE CONTROL 11 (I-014): no same-cycle bypass. Lane 1 reads the map as of
  // the start of the cycle, so a source naming lane 0's destination resolves to
  // the mapping lane 0 is superseding -- a tag whose producer has been renamed
  // away and which nobody will ever write. The consumer then waits forever on a
  // result that cannot arrive.
  assign rs3_hits_lane0 = 1'b0;
  assign rs4_hits_lane0 = 1'b0;
`else
  assign rs3_hits_lane0 = alloc_new_valid && (rs3_addr == alloc_rd);
  assign rs4_hits_lane0 = alloc_new_valid && (rs4_addr == alloc_rd);
`endif

  always_comb begin
    // Lane 0's sources, and readiness = "the producer has written its value
    // back". x0 is ready because there is nothing to read, and it returns the
    // zero identity rather than spec_map[0], which is a mapping x0 does not have.
    rs1_is_x0 = (rs1_addr == 5'd0);
    rs2_is_x0 = (rs2_addr == 5'd0);
    rs1_tag   = spec_map[rs1_addr];
    rs2_tag   = spec_map[rs2_addr];
    rs1_gen   = spec_gen[rs1_addr];
    rs2_gen   = spec_gen[rs2_addr];
    rs1_ready = rs1_is_x0 || wb_done[rs1_tag];
    rs2_ready = rs2_is_x0 || wb_done[rs2_tag];
    if (rs1_is_x0) begin
      rs1_tag = {REN_TAG_W{1'b0}};
      rs1_gen = {REN_GEN_W{1'b0}};
    end
    if (rs2_is_x0) begin
      rs2_tag = {REN_TAG_W{1'b0}};
      rs2_gen = {REN_GEN_W{1'b0}};
    end

    // Lane 1's sources. The bypass takes precedence over the map, and its
    // readiness is 0 by construction: the bypass *means* the producer has not
    // written back yet.
    rs3_is_x0  = (rs3_addr == 5'd0);
    rs4_is_x0  = (rs4_addr == 5'd0);
    rs3_bypass = rs3_hits_lane0;
    rs4_bypass = rs4_hits_lane0;
    if (rs3_bypass) begin
      rs3_tag = alloc_new_tag;
      rs3_gen = alloc_new_gen;
    end else begin
      rs3_tag = spec_map[rs3_addr];
      rs3_gen = spec_gen[rs3_addr];
    end
    if (rs4_bypass) begin
      rs4_tag = alloc_new_tag;
      rs4_gen = alloc_new_gen;
    end else begin
      rs4_tag = spec_map[rs4_addr];
      rs4_gen = spec_gen[rs4_addr];
    end
    rs3_ready = rs3_is_x0 || (!rs3_bypass && wb_done[rs3_tag]);
    rs4_ready = rs4_is_x0 || (!rs4_bypass && wb_done[rs4_tag]);
    if (rs3_is_x0) begin
      rs3_tag = {REN_TAG_W{1'b0}};
      rs3_gen = {REN_GEN_W{1'b0}};
    end
    if (rs4_is_x0) begin
      rs4_tag = {REN_TAG_W{1'b0}};
      rs4_gen = {REN_GEN_W{1'b0}};
    end
  end

  // ---------------------------------------------------------------- writeback
  logic wb_in_range;

  assign wb_in_range = (wb_tag < REN_TAG_W'(REN_ENTRIES));

  // "Stale" means: not the tag's current owner. That covers a recycled tag with
  // an old generation, a tag that has been freed since, a tag that was never
  // allocated since reset, and a tag outside the bank partition.
`ifdef MOSAIC_RENAME_MUTANT_NO_GEN_CHECK
  // NEGATIVE CONTROL 1: the generation is not compared. A late writeback aimed
  // at a recycled tag overwrites the new owner's value with a dead one.
  assign wb_stale = wb_valid && (!wb_in_range || !gen_valid[wb_tag] || free_bits[wb_tag]);
`else
  assign wb_stale = wb_valid && (!wb_in_range || !gen_valid[wb_tag] || free_bits[wb_tag] ||
                                 (wb_gen != gen[wb_tag]));
`endif

`ifdef MOSAIC_RENAME_MUTANT_NO_DUP_WB_GUARD
  // NEGATIVE CONTROL 5: the "already written" guard is gone, so a second
  // producer for the same destination is accepted -- the result is written twice
  // and, worse, a duplicate broadcast from one lane can land in another
  // owner's register.
  assign wb_duplicate = 1'b0;
  assign wb_accepted  = wb_valid && !wb_stale;
`else
  assign wb_duplicate = wb_valid && !wb_stale && wb_done[wb_tag];
  assign wb_accepted  = wb_valid && !wb_stale && !wb_done[wb_tag];
`endif

  // ------------------------------------------------------------------- free
  logic free_in_range;

  // The journal stores at most REN_ROB entries, so an entry address is one bit
  // narrower than the length, which also has to be able to say "full".
  localparam int unsigned REN_JIDX_W = (REN_ROB <= 1) ? 1 : $clog2(REN_ROB);

  assign free_in_range = (free_tag < REN_TAG_W'(REN_ENTRIES));

`ifdef MOSAIC_RENAME_MUTANT_NO_GEN_CHECK
  assign free_stale = free_valid && (!free_in_range || !gen_valid[free_tag]);
`else
  assign free_stale = free_valid && (!free_in_range || !gen_valid[free_tag] ||
                                     (free_gen != gen[free_tag]));
`endif

`ifdef MOSAIC_RENAME_MUTANT_NO_DOUBLE_FREE_CHECK
  // NEGATIVE CONTROL 6: releasing a tag that is already free is accepted, so
  // the free set gains a tag it does not own and a later allocation hands out
  // a register that is still live.
  assign free_double   = 1'b0;
  assign free_accepted = free_valid && !free_stale;
`else
  assign free_double   = free_valid && !free_stale && free_bits[free_tag];
  assign free_accepted = free_valid && !free_stale && !free_bits[free_tag];
`endif

  // ----------------------------------------------------------------- commit
  // A commit for x0 writes no architectural state and therefore frees nothing.
  assign commit_x0_dropped = commit_valid && (commit_rd == 5'd0);
  assign commit_accepted   = commit_valid && (commit_rd != 5'd0);

  // The mapping this commit supersedes. It is freed here and nowhere else, and
  // the identity check makes a repeated commit of the same mapping a no-op
  // instead of a free of the mapping that was just installed.
  logic commit_supersedes;
  assign commit_supersedes = commit_accepted &&
                             ((cmt_map[commit_rd] != commit_tag) ||
                              (cmt_gen[commit_rd] != commit_gen));

  // ------------------------------------------------- the second commit lane
  // The identical rule, one instruction further on in program order. Two commits
  // to the same rd in one cycle are the case the lane exists for: lane 0
  // installs its mapping and releases whatever rd pointed at before, lane 1
  // installs the younger mapping and releases lane 0's tag. That is correct
  // because by the time lane 1 commits, lane 0's tag really is superseded -- the
  // architectural value of rd after the cycle is lane 1's, and lane 0's tag has
  // no reader left.
  assign commit2_x0_dropped = commit2_valid && (commit2_rd == 5'd0);
  assign commit2_accepted   = commit2_valid && (commit2_rd != 5'd0);

  logic commit2_supersedes;

  // The committed map **as lane 0 leaves it**: the map lane 1 must compare
  // against, and the map its release of the superseded tag is read from.
  //
  // It is a separate signal rather than a read of `cmt_map_q`, and that is not
  // style. `cmt_map_q` is written by lane 1 *later in the same combinational
  // block*, and a continuous assign reading a variable assigned in an
  // always_comb sees the settled (post-block) value -- measured, not assumed:
  // a minimal always_comb that sets a bit and a continuous assign that reads it
  // observes the bit set. So `cmt_map_q[commit2_rd]` is, whenever lane 1 is
  // accepted, exactly `commit2_tag`: the "supersedes" test would compare
  // commit2_tag with itself, be false on every cycle, and lane 1 would release
  // *nothing* -- one physical tag leaked per two-wide retirement, visible only
  // dozens of instructions later as spurious exhaustion. Reading the
  // intermediate map explicitly is what makes "lane 1 sees the map as lane 0
  // left it" true instead of merely intended.
  logic [REN_TAG_W-1:0] cmt_map_l0 [REN_ARCH_REGS];
  logic [REN_GEN_W-1:0] cmt_gen_l0 [REN_ARCH_REGS];

  always_comb begin
    for (int unsigned a = 0; a < REN_ARCH_REGS; a++) begin
      cmt_map_l0[a] = cmt_map[a];
      cmt_gen_l0[a] = cmt_gen[a];
    end
    if (commit_accepted) begin
      cmt_map_l0[commit_rd] = commit_tag;
      cmt_gen_l0[commit_rd] = commit_gen;
    end
  end

  // Against the map as lane 0 left it, not against the pre-edge map: the mapping
  // lane 1 supersedes is the one lane 0 just installed whenever both lanes write
  // the same rd. Against the pre-edge map both lanes would name one superseded
  // tag and lane 0's tag would never come back -- the leak the mutant below
  // injects on purpose.
`ifdef MOSAIC_RENAME_MUTANT_WAW_COMMIT2_PRE_MAP
  // NEGATIVE CONTROL 10 (I-014): the second commit lane works from the
  // *pre-lane-0* committed map -- both the comparison and the tag it releases.
  // For a WAW pair both lanes then release the same superseded mapping (lane 0's
  // old tag, twice, which a set absorbs) and the mapping lane 1 actually
  // superseded (lane 0's new tag) is never released at all: the tag leaks and the
  // free list disagrees with the ROB.
  //
  // Both halves are needed for the control to be a control. Changing only the
  // comparison leaves the release still aimed at `cmt_map_l0`, which for a WAW
  // pair is exactly the right tag, so the mutant would be *behaviourally
  // identical* to the shipping build and would prove nothing -- the trap this
  // project has hit before.
  assign commit2_supersedes = commit2_accepted &&
                              ((cmt_map[commit2_rd] != commit2_tag) ||
                               (cmt_gen[commit2_rd] != commit2_gen));
`else
  assign commit2_supersedes = commit2_accepted &&
                              ((cmt_map_l0[commit2_rd] != commit2_tag) ||
                               (cmt_gen_l0[commit2_rd] != commit2_gen));
`endif

  // --------------------------------------------------------------- recovery
  // A checkpoint is a usable recovery point only where the speculative map
  // equals the committed map. That equality is the checkable form of "the
  // redirecting branch is the oldest outstanding instruction": an instruction
  // that has allocated a tag and not yet committed has a speculative mapping
  // that differs from the committed one for its rd, so `spec == cmt` for every
  // architectural register means no such instruction is outstanding.
  //
  // It has to be checked because the restore is `spec := cmt`: at a checkpoint
  // taken with older register-writers still in flight, that assignment discards
  // *their* mappings -- and their tags stay allocated, because they were
  // allocated before the checkpoint and are therefore not in the undo window.
  // The result is a physical tag neither free nor named by any mapping: exactly
  // the state the ownership invariant forbids, reached by a legal-looking
  // sequence of requests.
  logic spec_eq_cmt;

  always_comb begin
    spec_eq_cmt = 1'b1;
    for (int unsigned a = 0; a < REN_ARCH_REGS; a++) begin
      if ((spec_map[a] != cmt_map[a]) || (spec_gen[a] != cmt_gen[a])) begin
        spec_eq_cmt = 1'b0;
      end
    end
  end

  // Reported every cycle, so a caller can sample it in the cycle it takes a
  // checkpoint rather than discovering the problem at the mispredict.
  assign ckpt_committed = spec_eq_cmt;

  assign squash_underflow = squash && !ckpt_seen;

`ifdef MOSAIC_RENAME_MUTANT_NO_BOUNDARY_CHECK
  // NEGATIVE CONTROL 13 (I-014): the boundary check is not enforced. A squash to
  // a checkpoint taken with older register-writers still in flight is accepted,
  // and the restore from the committed map silently loses their mappings while
  // their tags stay allocated and unreachable.
  assign squash_not_committed = 1'b0;
  assign squash_accepted      = squash && ckpt_seen;
`else
  assign squash_not_committed = squash && ckpt_seen && !ckpt_at_boundary;
  assign squash_accepted      = squash && ckpt_seen && ckpt_at_boundary;
`endif

  assign journal_overflow = j_overflow;

  // How many journal entries a squash has to undo, newest first. The length is
  // clamped to the journal size; the overflow flag is the report that the clamp
  // happened, so a caller can never mistake a partial restore for a complete
  // one.
  logic [REN_JLEN_W-1:0] undo_n;

  always_comb begin
    if (squash_accepted) begin
      undo_n = j_len;
    end else begin
      undo_n = {REN_JLEN_W{1'b0}};
    end
    if (undo_n > REN_JLEN_W'(REN_ROB)) begin
      undo_n = REN_JLEN_W'(REN_ROB);
    end
  end

  // How many entries the next-state block actually applies. It equals `undo_n`
  // in every build except the negative control that removes the free-set
  // restore, and keeping it a separate signal is what lets that control be a
  // one-line change instead of a second driver on `undo_n`.
  logic [REN_JLEN_W-1:0] undo_apply;

  // ------------------------------------------------------- next-state: masks
  // The free set, the generation table and the written flags are all computed
  // as whole next-state vectors rather than as a pile of per-event register
  // writes. It costs one 96-bit adder tree for the occupancy and it removes
  // every question about what happens when two events in one cycle touch the
  // same entry.
  logic [REN_ENTRIES-1:0] free_q;
  logic [REN_ENTRIES-1:0] genv_q;
  logic [REN_GEN_W-1:0]   gen_q  [REN_ENTRIES];
  logic [REN_ENTRIES-1:0] wbd_q;

  always_comb begin
    free_q = free_bits;
    genv_q = gen_valid;
    wbd_q  = wb_done;
    for (int unsigned e = 0; e < REN_ENTRIES; e++) begin
      gen_q[e] = gen[e];
    end

    // 1. The group's allocations, lane 0 then lane 1. The two tags are distinct
    //    by construction -- lane 1's scan cannot see lane 0's tag -- so the order
    //    matters only for the map below, not for these writes: clearing a set bit
    //    twice and stepping one generation twice would be a different (and
    //    wrong) statement, which is why the second scan has to exclude the first
    //    tag rather than reuse it.
    if (alloc_new_valid) begin
      free_q[scan_tag] = 1'b0;
      gen_q[scan_tag]  = gen_valid[scan_tag] ? (gen[scan_tag] + REN_GEN_W'(1))
                                              : {REN_GEN_W{1'b0}};
      genv_q[scan_tag] = 1'b1;
      wbd_q[scan_tag]  = 1'b0;
    end
    if (alloc2_new_valid) begin
      free_q[alloc2_new_tag] = 1'b0;
      gen_q[alloc2_new_tag]  = alloc2_new_gen;
      genv_q[alloc2_new_tag] = 1'b1;
      wbd_q[alloc2_new_tag]  = 1'b0;
    end

    // 2. An explicit release, and the mapping a commit supersedes, both put a
    //    tag back. Neither changes the generation: the generation counts
    //    allocations, so a tag that goes free and is handed out again comes
    //    back with a larger number and an old writeback cannot match it.
    if (free_accepted) begin
      free_q[free_tag] = 1'b1;
    end
    if (commit_supersedes) begin
      free_q[cmt_map[commit_rd]] = 1'b1;
    end
    // Lane 1 releases what *lane 1* superseded, which in the same-rd case is
    // lane 0's tag -- hence `cmt_map_l0`, the map as lane 0 left it, and not
    // `cmt_map_q`, which by then also carries lane 1's own install.
    if (commit2_supersedes) begin
`ifdef MOSAIC_RENAME_MUTANT_WAW_COMMIT2_PRE_MAP
      free_q[cmt_map[commit2_rd]] = 1'b1;
`else
      free_q[cmt_map_l0[commit2_rd]] = 1'b1;
`endif
    end

    // 3. The squash undoes every allocation made after the checkpoint, oldest
    //    entry first so the newest is applied last. Allocation is refused in a
    //    squash cycle, so this can never race with step 1.
    //
    //    The free set is a set, so putting a tag back is an idempotent write and
    //    its order does not matter. The generation is not a set, so its undo is
    //    the exact inverse of the allocation's step:
    //
    //        allocate:  gen = prev_valid ? gen + 1 : 0,  gen_valid = 1
    //        undo:      gen = prev_valid ? gen - 1 : 0,  gen_valid = prev_valid
    //
    //    One bit per journal entry is therefore the whole undo state, and the
    //    undo is exact for any interleaving of frees and re-allocations inside
    //    the window: nothing else ever writes a generation, so "gen - 1" is the
    //    value that allocation found, however many later allocations of the same
    //    tag came after it.
`ifdef MOSAIC_RENAME_MUTANT_NO_FREE_RESTORE
    // NEGATIVE CONTROL 3: the free set is not restored. The speculative map goes
    // back to the committed map, but every tag allocated after the checkpoint
    // stays marked allocated -- so the free count leaks one tag per squashed
    // instruction and the register file quietly shrinks. The journal is still
    // consumed, so the free count also stops matching the live mappings: the
    // shadow model catches it on the first cycle after the squash.
    undo_apply = {REN_JLEN_W{1'b0}};
`else
    undo_apply = undo_n;
`endif
    for (int unsigned k = 0; k < REN_ROB; k++) begin
      if (REN_JLEN_W'(k) < undo_apply) begin
        free_q[j_tag[REN_JIDX_W'(k)]] = 1'b1;
        gen_q[j_tag[REN_JIDX_W'(k)]]  =
            j_prev_valid[REN_JIDX_W'(k)]
              ? (gen[j_tag[REN_JIDX_W'(k)]] - REN_GEN_W'(1))
              : {REN_GEN_W{1'b0}};
        genv_q[j_tag[REN_JIDX_W'(k)]] = j_prev_valid[REN_JIDX_W'(k)];
      end
    end

    // 4. The owner that made it back writes.
    if (wb_accepted) begin
      wbd_q[wb_tag] = 1'b1;
    end
  end

  // ---------------------------------------------------- next-state: the maps
  always_comb begin
    for (int unsigned a = 0; a < REN_ARCH_REGS; a++) begin
      spec_map_q[a] = spec_map[a];
      spec_gen_q[a] = spec_gen[a];
      cmt_map_q[a]  = cmt_map[a];
      cmt_gen_q[a]  = cmt_gen[a];
    end

    // A commit is permanent, so it lands first and the squash restore reads the
    // post-commit committed map. Lane 1 lands immediately after lane 0 and
    // still before the restore, so a two-wide retirement that lands in a squash
    // cycle reaches the speculative map through the committed map rather than
    // being lost.
    if (commit_accepted) begin
      cmt_map_q[commit_rd] = commit_tag;
      cmt_gen_q[commit_rd] = commit_gen;
    end

    if (commit2_accepted) begin
      cmt_map_q[commit2_rd] = commit2_tag;
      cmt_gen_q[commit2_rd] = commit2_gen;
    end

    if (squash_accepted) begin
      for (int unsigned a = 0; a < REN_ARCH_REGS; a++) begin
        spec_map_q[a] = cmt_map_q[a];
        spec_gen_q[a] = cmt_gen_q[a];
      end
    end

    if (alloc_new_valid) begin
      spec_map_q[alloc_rd] = scan_tag;
      spec_gen_q[alloc_rd] = alloc_new_gen;
    end

    // Lane 1 lands on the speculative map *after* lane 0, so a WAW pair ends
    // with lane 1's mapping: the younger macro wins the architectural register,
    // exactly as two sequential renames would have left it. The two lanes write
    // distinct tags, so the only entry they can both touch is the same rd, and
    // there the later write is the correct one.
    if (alloc2_new_valid) begin
      spec_map_q[alloc2_rd] = alloc2_new_tag;
      spec_gen_q[alloc2_rd] = alloc2_new_gen;
    end
  end

  // ---------------------------------------------------------- journal control
  logic [REN_JLEN_W-1:0] j_len_q;
  logic                  j_overflow_q;

  // The undo window advances by one entry per allocating lane, and the group's
  // two entries go in lane order. `j_push0`/`j_push1` are the same conditions the
  // register block uses, evaluated here so the slot arithmetic is written once:
  // lane 1's entry is one past lane 0's when lane 0 has one, and at the tail when
  // lane 0 allocated nothing (an x0 lane), which is what keeps the window a
  // contiguous run in allocation order.
  //
  // `j_tail` is the index this cycle's first entry lands on. A checkpoint empties
  // the window, and it does so *before* this cycle's entries are appended: the
  // recovery point is the state at the start of the checkpoint cycle, so an
  // allocation made in that same cycle is younger than the checkpoint and must be
  // undone by a squash to it. Dropping it instead (the pre-I-014 rule) left its
  // tag allocated while the restored map named nothing -- one tag neither free
  // nor owned per checkpoint-and-allocate cycle.
  logic                  j_push0;
  logic                  j_push1;
  logic [REN_JIDX_W-1:0] j_idx1;
  logic [REN_JLEN_W-1:0] j_tail;

  assign j_tail = (ckpt_valid && !squash) ? {REN_JLEN_W{1'b0}} : j_len;

`ifdef MOSAIC_RENAME_MUTANT_CKPT_ALLOC_LEAK
  // NEGATIVE CONTROL 12 (I-014): a checkpoint cycle's allocations are not
  // journalled (the pre-I-014 rule). The recovery point is then the *end* of the
  // checkpoint cycle while the free set is restored to its start, so the tag the
  // group took is neither free nor named by any mapping after a squash.
  assign j_push0 = alloc_new_valid && !ckpt_valid && (j_len < REN_JLEN_W'(REN_ROB));
  assign j_idx1  = REN_JIDX_W'(j_push0 ? (j_len + REN_JLEN_W'(1)) : j_len);
  assign j_push1 = alloc2_new_valid && !ckpt_valid &&
                   (REN_JLEN_W'(j_idx1) < REN_JLEN_W'(REN_ROB));
`else
  assign j_push0 = alloc_new_valid && (j_tail < REN_JLEN_W'(REN_ROB));
  assign j_idx1  = REN_JIDX_W'(j_push0 ? (j_tail + REN_JLEN_W'(1)) : j_tail);
  assign j_push1 = alloc2_new_valid && (REN_JLEN_W'(j_idx1) < REN_JLEN_W'(REN_ROB));
`endif

  always_comb begin
    j_len_q      = j_tail;
    j_overflow_q = j_overflow;

    // Lane 1's entry is counted only if lane 0's fit. A window that held a later
    // entry while missing an earlier one could not be undone: the undo walks
    // oldest first, and it would step a generation down without the step that
    // made it go up.
    if (alloc_new_valid) begin
      if (j_len_q < REN_JLEN_W'(REN_ROB)) begin
        j_len_q = j_len_q + REN_JLEN_W'(1);
      end else begin
        // The undo bound was violated. Reported, not silently absorbed: the
        // alternative is a squash that restores less than the truth while
        // reporting success.
        j_overflow_q = 1'b1;
      end
    end

    if (alloc2_new_valid) begin
      if (j_len_q < REN_JLEN_W'(REN_ROB)) begin
        j_len_q      = j_len_q + REN_JLEN_W'(1);
      end else begin
        j_overflow_q = 1'b1;
      end
    end

    if (squash_accepted) begin
      j_len_q = {REN_JLEN_W{1'b0}};
    end
  end

  // -------------------------------------------------------------- registers
  always_ff @(posedge clk) begin
    if (rst) begin
      for (int unsigned a = 0; a < REN_ARCH_REGS; a++) begin
        spec_map[a] <= REN_TAG_W'(a);
        spec_gen[a] <= {REN_GEN_W{1'b0}};
        cmt_map[a]  <= REN_TAG_W'(a);
        cmt_gen[a]  <= {REN_GEN_W{1'b0}};
      end
      // Tags 0..ARCH_REGS-1 are the architectural reset mappings and are
      // therefore owned, not free. See the reset section of the header.
      free_bits  <= init_owned_mask();
      gen_valid  <= {REN_ENTRIES{1'b0}};
      wb_done    <= {REN_ENTRIES{1'b0}};
      // The rotation point starts at the first allocatable tag, so the machine's
      // first allocation takes the lowest free tag instead of scanning past the
      // reserved range.
      alloc_ptr  <= REN_TAG_W'(REN_ARCH_REGS);
      j_len      <= {REN_JLEN_W{1'b0}};
      ckpt_seen  <= 1'b0;
      ckpt_at_boundary <= 1'b0;
      j_overflow <= 1'b0;
    end else begin
      for (int unsigned a = 0; a < REN_ARCH_REGS; a++) begin
        spec_map[a] <= spec_map_q[a];
        spec_gen[a] <= spec_gen_q[a];
        cmt_map[a]  <= cmt_map_q[a];
        cmt_gen[a]  <= cmt_gen_q[a];
      end
      free_bits <= free_q;
      gen_valid <= genv_q;
      wb_done   <= wbd_q;
      for (int unsigned e = 0; e < REN_ENTRIES; e++) begin
        gen[e] <= gen_q[e];
      end

      // The rotation point follows the last allocation, so a fully free list
      // hands the tags out in 0,1,2,... and wraps after ENTRIES allocations. A
      // two-wide group therefore leaves the pointer exactly where two
      // single-width allocations would have left it.
      if (alloc2_new_valid) begin
        alloc_ptr <= (alloc2_new_tag == REN_TAG_W'(REN_ENTRIES - 1))
                     ? {REN_TAG_W{1'b0}} : (alloc2_new_tag + REN_TAG_W'(1));
      end else if (alloc_new_valid) begin
        alloc_ptr <= (scan_tag == REN_TAG_W'(REN_ENTRIES - 1)) ? {REN_TAG_W{1'b0}}
                                                              : (scan_tag + REN_TAG_W'(1));
      end

      // One journal entry per allocation, holding the state that allocation
      // replaced. The previous generation itself is not stored: the undo is the
      // exact inverse of the allocation's increment, so one bit is the whole
      // undo state. A two-wide group pushes lane 0's entry and then lane 1's, so
      // the window stays in allocation order and the undo can walk it oldest
      // first -- which is the property the generation rollback depends on.
      if (j_push0) begin
        j_tag[REN_JIDX_W'(j_tail)] <= scan_tag;
        j_prev_valid[REN_JIDX_W'(j_tail)] <= gen_valid[scan_tag];
      end
      if (j_push1) begin
        j_tag[j_idx1] <= alloc2_new_tag;
        j_prev_valid[j_idx1] <= gen_valid[alloc2_new_tag];
      end

      j_len      <= j_len_q;
      j_overflow <= j_overflow_q;
      if (ckpt_valid && !squash) begin
        ckpt_seen <= 1'b1;
        // The recovery point is the state at the start of this cycle, so the
        // boundary test is the pre-edge comparison, not one made after the
        // cycle's own allocations have landed.
        ckpt_at_boundary <= spec_eq_cmt;
      end
    end
  end

  // -------------------------------------------------------------- reporting
  assign free_count = count_free(free_bits);

  assign dbg_free_mask = free_bits;
  assign dbg_gen_valid = gen_valid;
  assign dbg_wb_done   = wb_done;
  assign dbg_j_len     = j_len;

  for (genvar g = 0; g < REN_ENTRIES; g++) begin : g_dbg_tag_gen
    assign dbg_tag_gen[g*REN_GEN_W +: REN_GEN_W] = gen[g];
  end

  for (genvar m = 0; m < REN_ARCH_REGS; m++) begin : g_dbg_map
    assign dbg_spec_map[m*REN_MAP_W +: REN_MAP_W] = {spec_gen[m], spec_map[m]};
    assign dbg_cmt_map[m*REN_MAP_W +: REN_MAP_W]  = {cmt_gen[m], cmt_map[m]};
  end

endmodule : mosaic_rename

`resetall
`default_nettype wire
