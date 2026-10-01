// ============================================================================
// mosaic_recovery -- work package I-018: branch checkpoints and precise
// recovery.
//
// The problem this module exists to solve is one sentence long: when a branch
// resolves wrong, the machine has to be put back **exactly** where it was when
// that branch was dispatched, and not somewhere nearby. "Nearby" is the whole
// failure. A recovery that rewinds the free list but leaves the generation
// table ahead produces a tag whose stale writeback is accepted. One that
// rewinds the free list too far hands a live register to two owners. One that
// rewinds the generation table but not the free list leaks a register per
// mispredict, and the machine eventually stalls with an empty free list it
// believes is full. None of those announce itself: they surface dozens of
// instructions later as something else entirely.
//
// ------------------------------------------------ the choice of method
//
// The card offers two: "committed map + surviving-prefix rebuild" or "full
// checkpoint". This module takes the **full checkpoint**, and the reason is
// arithmetic rather than taste.
//
// A surviving-prefix rebuild needs a commit boundary to rebuild from: the
// committed map, the free list at that boundary, and every allocation after it
// held in an undo log so the prefix can be replayed. That log is the same undo
// journal this module needs anyway, and the replay is the same walk. The full
// checkpoint keeps, per outstanding branch, a copy of the speculative RAT layer
// and restores by copy-back, and pays for the free list and the generation
// table by replaying the journal *backwards*. So the cost per checkpoint is one
// 32-entry map copy rather than a whole machine copy, and the expensive
// structure -- the journal -- is shared by every checkpoint rather than
// duplicated per checkpoint.
//
// -------------------------------------------- what is checkpointed, and why
//
// A checkpoint holds everything a squash has to put back:
//
//   spec_map/spec_gen[32]  the speculative RAT layer. Without it a restored
//                          machine dispatches from a map that still names the
//                          tags the squashed instructions owned, and the first
//                          surviving instruction to read one of those registers
//                          reads a dead value.
//   tail                   the speculative ROB tail. The restored machine
//                          continues allocating from here, and this is the
//                          quantity the ROB-side flush point is derived from.
//   alloc_ptr              the rotation point of the free-list scan. Restoring
//                          the free *set* without restoring the scan position
//                          would hand out a different tag than the checkpoint
//                          would have: the set is right and the next allocation
//                          is still wrong, which is the failure this whole
//                          module is about.
//   epoch                  the fetch epoch at the checkpoint. It is recorded and
//                          exported, and the restore does **not** put it back:
//                          the redirect raises the epoch by one so that new
//                          reservations are distinguishable from the ones the
//                          squash cancels, and restoring the recorded value
//                          would undo exactly that. It is the baseline a
//                          consumer publishes with the redirect, not state the
//                          restore rewinds.
//   jmark                  the journal length at the checkpoint: where the undo
//                          stops.
//
// The free list, the generation table and the written flags are **not**
// checkpointed. They are replayed backwards from the journal, which is exact
// and costs one bit per journal entry rather than 96 bits per checkpoint.
// `cmt_map` is deliberately not checkpointed either, and that is the entire
// boundary with I-017 -- see "the I-017 / I-018 boundary" below.
//
// ------------------------------------------------------ the undo journal
//
// One entry per allocation: {tag, prev_gen_valid}. The generation undo is the
// exact inverse of the allocation's step,
//
//     allocate:  gen = prev_valid ? gen + 1 : 0,  gen_valid = 1
//     undo:      gen = prev_valid ? gen - 1 : 0,  gen_valid = prev_valid
//
// so one bit is the whole undo state and the undo is exact for any
// interleaving of frees and re-allocations inside the window: nothing else ever
// writes a generation, so "gen - 1" is the value allocation found, however many
// later allocations of the same tag came after it. The free set is a *set*, so
// putting a tag back is idempotent and the undo's order cannot matter.
//
// Why only allocations are journalled, and not frees: every free here is caused
// by a commit (the superseded committed mapping) or by an explicit release, and
// a commit is permanent. A commit after a checkpoint belongs to an instruction
// *older* than the checkpointing branch -- retire is in order, so if a younger
// instruction had committed, the branch would have committed too and there
// would be nothing to squash to -- and undoing it would resurrect a mapping the
// ISA has already published. And every allocation after a checkpoint belongs to
// an instruction younger than it, which cannot have committed. So the journal
// is exactly the set of events that can be undone.
//
// ------------------------------------------------- the bound, and its report
//
// The journal is bounded by `MOSAIC_ROB_ENTRIES`: at most ROB_ENTRIES
// instructions can be younger than a given checkpoint, and each contributes at
// most one entry. At p0 that is 64 entries against 96 - 32 = 64 allocatable
// tags -- exactly, with nothing to spare -- which is why
// `tools/check_profile.py` requires
// `int_prf.entries - arch_int_regs >= rob.entries`.
//
// Reaching the bound exactly is legal and is the *expected* steady state for a
// full queue. Exceeding it is not: the allocation is **refused** and
// `journal_overflow` reports it, and the window is never wrapped. Wrapping is
// the one response that is not available, because a wrapped entry overwrites a
// live one and the restore then returns a state the machine never passed
// through while reporting success.
//
// The consequence is a deliberate, named throttle: because a retire inside a
// window does not retire journal entries, a checkpoint left open across many
// retires can reach the bound sooner than the occupancy suggests. The
// allocation is refused and reported rather than corrupting the journal. That is
// the conservative direction: a stalled allocation is visible, a wrapped journal
// is not.
//
// ------------------------------------------------- oldest redirect wins
//
// Redirects arrive from resolve, out of order, and two of them can arrive in one
// cycle (one per cluster). Two pending redirects are not interchangeable:
// taking the younger fetches from a path the older redirect is about to
// discard, and the machine re-fetches, re-allocates and re-squashes against a
// state that has already moved. The card names the consequence -- it can wedge.
//
// So the arbiter selects by **age**, and age is the ROB generation of the
// redirecting instruction: the same monotonic, never-rewound counter that makes
// a recycled slot distinguishable from the macro that owns it. Generations
// increase with allocation order, so the oldest instruction has the smallest
// generation and the selection is an unsigned minimum over every live candidate
// -- the queue and this cycle's two resolve ports together, so a redirect
// arriving in the same cycle as an older pending one loses, and the rule is
// total rather than "priority encoder, oldest queue entry first".
//
// That unsigned compare is exact because of a stated bound: the spread between
// the oldest and newest live generation is at most ROB_ENTRIES allocations, and
// ROB_ENTRIES is 64 against a 12-bit counter's 4096 -- a 64x margin. The same
// argument mosaic_rob makes about its own stale window, applied to a different
// comparison.
//
// A redirect naming a generation with no live checkpoint is **stale**: its
// branch was squashed by an earlier recovery, so it describes a path that no
// longer exists. It is dropped and reported separately, because a taken-but-dead
// redirect is worse than a not-taken one and must not hide in the same counter.
// Every other live candidate when one is taken is *younger*, so the same squash
// kills it: those are counted in `redirect_killed` and never silently lost.
// The queue is emptied on a take, which is the same statement.
//
// Reachability, stated because it is a fact about the shipping build rather than
// a hope: a live, non-stale candidate is taken in the cycle it is presented, so
// nothing is ever deferred and the queue is never occupied -- `o_rdq_depth` is
// zero in every cycle, and the queue half of the candidate set never decides an
// arbitration. Deferring a *younger* redirect until an older **checkpoint**
// resolves is not the rule and could not be: an older branch is resolved by the
// redirect this unit would be refusing to take, so the deferral would be
// permanent. The queue is dead weight under the rule as stated and is listed as
// such in results/reports/I-018-recovery.md; it is left in place here because
// deleting a port is an interface change across the checkpoint consumers, which
// the integration lead is sequencing (see the report's split section).
//
// ----------------------------------- late responses, credit once, age-bounded
//
// A redirect kills an *age range*, not an epoch. It raises the epoch, and a
// response is matched against the reservation's identity by epoch, but whether
// the response's instruction was squashed is decided by the **owner generation**
// recorded when the credit was reserved: a reservation whose owner generation is
// at or above the redirecting branch's is cancelled, and one whose owner is older
// than the branch survives and its result still lands.
//
// That split is forced, not stylistic. An older load or DIV that is still in
// flight when a *younger* branch mispredicts was issued before the redirect, so
// it carries a pre-redirect epoch -- and it is not squashed, because the squash
// only removes work younger than the branch. Dropping everything from the
// previous epoch would discard the older instruction's result, and the older ROB
// head would then have a uop that can never complete: precisely the deadlock the
// I-018 card's blocking rule names ("kill every old-epoch instruction"), and
// precisely what `docs/implementation-plan.md` §1.3 forbids when it requires the
// PRF/result epoch check to respect the older instruction still live across a
// redirect. The same paragraph is why age, not the epoch, is the kill rule; the
// epoch is still needed, as the (slot, epoch) half of the identity that stops a
// response for a recycled slot from being read as the live reservation's.
//
// "Exactly once" is why the credit is a *table of slots* rather than a counter. A
// counter cannot tell a second delivery of a credit from the first. A table can:
// the first delivery finds the slot busy, returns the credit and clears the
// slot; a second delivery finds no slot and returns nothing. The credit is
// returned when the cancel is *acknowledged* -- the arrival of the response that
// is now stale -- and not at the redirect, because returning it at both ends is
// the mirror image of a credit leak and is exactly as fatal: the fabric
// over-issues and eventually has two producers writing one destination. The
// redirect latches `cred_cancel`, one bit per slot, and a response and the
// redirect that kills it in the same cycle still return the credit once: the
// classification applies the boundary combinationally for that cycle and the
// slot's own clear below removes the latch, so a later copy finds a free slot.
//
// This is the ABA hazard `config/contracts/interfaces.json` names for the fetch
// and execute interfaces, and the reason the identity is (slot, epoch) rather
// than the slot alone.
//
// -------------------------------------------------- I-017 / I-018 boundary
//
// I-017 (mosaic_retire) owns the committed map and `minstret`. I-018 owns *when*
// the speculative layer is rewound to it. Concretely:
//
//   * I-018 never rewinds the committed map. It applies commits, but only ever
//     as `cmt_map[rd] = the mapping being committed`, which is I-017's in-order
//     decision arriving on `commit_valid`. The restore reads the committed map
//     as it stands *after* this cycle's commit, and restoring over it would
//     resurrect the mapping the commit just replaced.
//   * I-018 drives `retire_block`, and it is high in exactly one situation: the
//     cycle a redirect is taken. So a retire and a recovery never land in the
//     same cycle, and the ordering of the two is total rather than
//     "sometimes both apply". A retire in *other* cycles while a checkpoint is
//     open is fine and needs no block: that commit is permanent, its free is
//     permanent, and its journal entry is below the oldest live checkpoint's
//     mark and so is never undone.
//   * I-018 consumes no signal from I-017 other than `commit_valid` and
//     `rob_retire`, so the two cases build and run independently.
//   * I-018 does not instantiate mosaic_rob or mosaic_rename. It owns the
//     speculative rename state and exports it read-only, which is what lets this
//     be a single-file unit test; at integration those exports become the state
//     the shared rename module reads.
//
// ------------------------------------------------ the ROB-side interface gap
//
// `mosaic_rob` exposes `flush_valid`, which drops everything at and above the
// head. A precise branch recovery needs to drop everything at and above the
// *redirecting branch*, keeping the older instructions still in flight. There is
// no such port today, and adding one to I-016 is not this package's to do.
//
// So this module does the part it can own exactly: it restores the tail and
// allocation pointer it holds, and publishes `o_rob_flush_from` /
// `o_rob_flush_from_valid`, the *requested* flush point. When `mosaic_rob`
// grows a `flush_from` port, that output drives it and this module needs no
// change. The gap is named here rather than papered over with a full flush,
// because a full flush presented as a precise recovery is precisely the defect
// this module exists to prevent.
//
// -------------------------------------------- the checkpoint release gap
//
// A checkpoint is consumed by the restore that squashes its branch, and by
// nothing else. No port says "the branch this checkpoint belongs to resolved
// correctly and committed", so the stack only fills: after CKPT_DEPTH live
// branches every later `ckpt_valid` is refused with `ckpt_refused`. Refusing is
// the honest, reported answer and the case exercises it, but a machine that can
// never checkpoint again after eight concurrent branches is not a machine. The
// release belongs with the same in-order decision that retires the branch, so
// the port to add is a branch identity (generation) alongside `commit_valid`,
// and the reason it cannot be inferred from what is here is the reason to state
// it: `rob_retire` is one bit with no generation, so "retire releases the oldest
// checkpoint" would release a checkpoint for a branch that has not necessarily
// retired, and dropping a live checkpoint is a restore to a point that does not
// exist.
//
// --------------------------------------------------------------- mutants
//
// -DMOSAIC_RECOVERY_MUTANT_<n> injects one defect used to prove the case can
// fail. The shipping build defines none of them; the table with real output is
// in results/reports/I-018-recovery.md.
// ============================================================================

`default_nettype none
`resetall

/* verilator lint_off UNUSEDPARAM */
// The generated header declares one localparam per configuration knob for the
// whole project. This module names the rename/ROB subset; the rest belong to
// other modules and are unused *here* by construction, not by omission.
`include "mosaic_cfg_pkg.svh"
/* verilator lint_on UNUSEDPARAM */

// Widths are declared at file scope because a module's port list cannot see
// declarations inside its own body. Every one is derived from the generated
// package and nothing else, so there is no second copy of the geometry.
localparam int unsigned REC_ENTRIES   = mosaic_cfg_pkg::MOSAIC_INT_PRF_ENTRIES;
localparam int unsigned REC_TAG_W     = mosaic_cfg_pkg::MOSAIC_INT_PRF_TAG_W;
localparam int unsigned REC_GEN_W     = mosaic_cfg_pkg::MOSAIC_INT_PRF_TAG_W;
localparam int unsigned REC_ARCH      = mosaic_cfg_pkg::MOSAIC_ARCH_INT_REGS;
localparam int unsigned REC_ROB       = mosaic_cfg_pkg::MOSAIC_ROB_ENTRIES;
localparam int unsigned REC_ROB_IDX_W = mosaic_cfg_pkg::MOSAIC_ROB_INDEX_W;
localparam int unsigned REC_ID_W      = 2 * mosaic_cfg_pkg::MOSAIC_ROB_INDEX_W;
localparam int unsigned REC_XLEN      = mosaic_cfg_pkg::MOSAIC_XLEN;

module mosaic_recovery #(
    // Checkpoint depth: the number of branches that may hold a live checkpoint
    // at once. Not the journal bound -- that is ROB_ENTRIES, and it is a
    // different quantity. A profile wanting more outstanding branches would
    // deepen this; the journal would not follow.
    parameter int unsigned CKPT_DEPTH = 8,
    // Pending-redirect queue depth. Two redirects can arrive in one cycle, so
    // this is at least 2 for the same-cycle rule to be reachable at all.
    parameter int unsigned RDQ_DEPTH = 8,
    // Reserved result credits: the number of in-flight results a redirect can
    // cancel and whose credits can therefore be in flight.
    parameter int unsigned CRED_ENTRIES = 16,

    // Derived, and therefore not overridable: a caller can change CKPT_DEPTH but
    // can never create a width that disagrees with it. Every $clog2 is guarded
    // because $clog2(1) is zero bits, which would be a zero-width select.
    localparam int unsigned REC_CKPT_W  = (CKPT_DEPTH <= 1) ? 1 : $clog2(CKPT_DEPTH),
    localparam int unsigned REC_RDQ_W   = (RDQ_DEPTH <= 1) ? 1 : $clog2(RDQ_DEPTH),
    localparam int unsigned REC_CRED_W  = (CRED_ENTRIES <= 1) ? 1 : $clog2(CRED_ENTRIES),
    localparam int unsigned REC_PICK_W  = ((RDQ_DEPTH + 2) <= 1) ? 1 : $clog2(RDQ_DEPTH + 2),
    localparam int unsigned REC_MAP_W   = REC_TAG_W + REC_GEN_W,
    localparam int unsigned REC_FCNT_W  = REC_TAG_W + 1,   // must represent ENTRIES
    localparam int unsigned REC_EPOCH_W = REC_ROB_IDX_W + 1,
    localparam int unsigned REC_CNT_W   = $clog2(REC_ROB + 1),
    localparam int unsigned REC_JIDX_W  = (REC_ROB <= 1) ? 1 : $clog2(REC_ROB)
) (
    input  logic                          clk,
    input  logic                          rst,

    // ================================================================ allocate
    // One instruction allocating one destination. `alloc_rob_gen` is the ROB
    // generation of the instruction this destination belongs to; a later
    // redirect names the same value, so allocation and recovery agree on
    // identity by construction rather than by convention.
    input  logic                          alloc_valid,
    input  logic [4:0]                    alloc_rd,
    // The ROB generation of the instruction this destination belongs to. It is
    // recorded so the *order* of allocations is checked here rather than assumed
    // by the arbiter downstream: the redirect arbiter selects the oldest
    // candidate by unsigned minimum over this counter, and that minimum is only
    // "oldest" while the counter increases. A producer that presents
    // generations out of order breaks the arbiter's premise, so it is reported
    // rather than absorbed.
    input  logic [REC_ID_W-1:0]           alloc_rob_gen,
    output logic                          alloc_gen_regress,
    output logic                          alloc_accepted,
    output logic                          alloc_squashed,    // refused: a squash owns the cycle
    output logic                          alloc_exhausted,  // refused: the free set is empty
    output logic                          alloc_is_x0,       // accepted, and it was a write to x0
    output logic                          alloc_new_valid,   // a physical tag was allocated
    output logic [REC_TAG_W-1:0]          alloc_new_tag,
    output logic [REC_GEN_W-1:0]          alloc_new_gen,
    output logic                          alloc_old_valid,
    output logic [REC_TAG_W-1:0]          alloc_old_tag,
    output logic [REC_GEN_W-1:0]          alloc_old_gen,
    output logic                          alloc_journal_full,  // refused: the undo bound is hit
    output logic                          journal_overflow,    // the undo bound was violated

    // ============================================================== checkpoint
    // A checkpoint is taken at the *allocation* of a control transfer, in the
    // same cycle as that transfer's destination allocation if it has one. The
    // two ports are separate because a conditional branch takes a checkpoint
    // and allocates nothing, and conflating them would make every branch burn a
    // physical register.
    input  logic                          ckpt_valid,
    input  logic [REC_ID_W-1:0]           ckpt_rob_gen,
    output logic                          ckpt_accepted,
    output logic                          ckpt_refused,     // the checkpoint stack is full
    output logic [REC_CKPT_W:0]           o_ckpt_depth,

    // =================================================================== commit
    // In-order retire of an architectural register write, from I-017. Applied
    // unconditionally and before the restore: a commit is permanent, and a
    // commit offered in a squash cycle must be visible in the committed map the
    // restore reads.
    input  logic                          commit_valid,
    input  logic [4:0]                    commit_rd,
    input  logic [REC_TAG_W-1:0]          commit_tag,
    input  logic [REC_GEN_W-1:0]          commit_gen,
    output logic                          commit_accepted,
    output logic                          commit_x0_dropped,

    // The ROB's in-order retire acknowledgement, consumed so that a retire and
    // a recovery never land in the same cycle. See the I-017/I-018 boundary.
    input  logic                          rob_retire,

    // ================================================================ writeback
    input  logic                          wb_valid,
    input  logic [REC_TAG_W-1:0]          wb_tag,
    input  logic [REC_GEN_W-1:0]          wb_gen,
    output logic                          wb_accepted,
    output logic                          wb_stale,
    output logic                          wb_duplicate,

    // ===================================================================== free
    input  logic                          free_valid,
    input  logic [REC_TAG_W-1:0]          free_tag,
    input  logic [REC_GEN_W-1:0]          free_gen,
    output logic                          free_accepted,
    output logic                          free_stale,
    output logic                          free_double,

    // ================================================================= redirect
    // Two resolve ports, one per cluster: two control transfers can resolve in
    // the same cycle, and the oldest-wins rule has to be *reachable* for it to
    // be a rule rather than a claim.
    input  logic                          redirect0_valid,
    input  logic [REC_ID_W-1:0]           redirect0_rob_gen,
    input  logic [REC_XLEN-1:0]           redirect0_pc,
    input  logic                          redirect0_is_fault,
    input  logic                          redirect1_valid,
    input  logic [REC_ID_W-1:0]           redirect1_rob_gen,
    input  logic [REC_XLEN-1:0]           redirect1_pc,
    input  logic                          redirect1_is_fault,

    // ============================================================ the recovery
    output logic                          redirect_taken,
    output logic [REC_ID_W-1:0]           redirect_taken_gen,
    output logic [REC_XLEN-1:0]           redirect_taken_pc,
    output logic                          redirect_taken_is_fault,
    output logic                          redirect_stale,      // named a dead branch
    output logic [REC_RDQ_W:0]            redirect_killed,     // pending, now squashed
    output logic [REC_RDQ_W:0]            o_rdq_depth,
    output logic [REC_PICK_W-1:0]         o_redirect_src,     // which source was taken

    // Action ports. `squash` and `retire_block` are the I-018 -> I-017 signals;
    // `o_rob_flush_from` is the precise flush point mosaic_rob does not yet
    // accept (see the interface-gap note in the header).
    output logic                          squash,
    output logic                          retire_block,
    output logic                          rob_flush_valid,
    output logic                          o_rob_flush_from_valid,
    output logic [REC_ROB_IDX_W-1:0]      o_rob_flush_from,
    output logic [REC_CKPT_W-1:0]         o_restore_ckpt,     // which checkpoint was restored

    // ================================================== epoch and credit return
    // A result credit is reserved per in-flight result. A redirect cancels the
    // results it kills; each cancelled result's credit comes back exactly once,
    // when the now-stale response arrives. `credits_outstanding` is defined as
    // the population count of the reservation table, so it cannot disagree with
    // the table it reports on.
    input  logic                          cred_req_valid,
    // The slot the producer would prefer. The unit grants the lowest free slot
    // and reports a conflict when the named one is already reserved, so a
    // producer that reuses an id is caught rather than being given a second
    // reservation it will later return twice.
    input  logic [REC_CRED_W-1:0]         cred_req_id,
    output logic                          cred_req_conflict,
    input  logic [REC_ID_W-1:0]           cred_req_rob_gen,
    output logic                          cred_req_ok,
    output logic                          cred_req_full,

    input  logic                          rsp_valid,
    input  logic [REC_CRED_W-1:0]         rsp_id,
    input  logic [REC_EPOCH_W-1:0]        rsp_epoch,
    output logic                          rsp_accepted,       // live: consumed the credit
    output logic                          rsp_dropped_stale,  // owner squashed: credit returned
    output logic                          rsp_dropped_dup,    // stale identity, or already returned
    output logic                          rsp_dropped_orphan, // no credit: never reserved
    output logic                          credit_return,
    output logic [REC_CRED_W:0]           credits_outstanding,
    output logic [REC_EPOCH_W-1:0]        o_epoch,

    // ============================================================== occupancy
    output logic [REC_FCNT_W-1:0]         free_count,

    // ===================================================== verification output
    // The whole state, so the unit test can compare it wholesale rather than a
    // projection of it. Read-only; nothing in the design consumes it.
    output logic [REC_ENTRIES-1:0]        dbg_free_mask,
    output logic [REC_ENTRIES-1:0]        dbg_gen_valid,
    output logic [REC_ENTRIES-1:0]        dbg_wb_done,
    output logic [REC_ENTRIES*REC_GEN_W-1:0]        dbg_tag_gen,
    output logic [REC_ARCH*(REC_MAP_W)-1:0]         dbg_spec_map,
    output logic [REC_ARCH*(REC_MAP_W)-1:0]         dbg_cmt_map,
    output logic [REC_ROB_IDX_W-1:0]      dbg_tail,
    output logic [REC_TAG_W-1:0]          dbg_alloc_ptr,
    output logic [REC_CNT_W-1:0]          dbg_j_len,
    output logic [CKPT_DEPTH-1:0]         dbg_ckpt_valid,
    output logic [CKPT_DEPTH*REC_CNT_W-1:0]         dbg_ckpt_jmark,
    output logic [CKPT_DEPTH*REC_ID_W-1:0]          dbg_ckpt_gen,
    output logic [CKPT_DEPTH*REC_ROB_IDX_W-1:0]     dbg_ckpt_tail,
    output logic [CKPT_DEPTH*REC_TAG_W-1:0]         dbg_ckpt_alloc_ptr,
    output logic [CKPT_DEPTH*REC_EPOCH_W-1:0]       dbg_ckpt_epoch,
    output logic                          dbg_inexact,

    // ================================================================ reports
    output logic [31:0]                   o_journal_entries,  // allocations journalled, ever
    output logic [31:0]                   o_restores,         // restores performed, ever
    output logic [31:0]                   o_ckpt_taken,       // checkpoints taken, ever
    output logic [31:0]                   o_credit_returns,   // credits returned, ever
    output logic [31:0]                   o_rsp_stale,        // stale responses dropped, ever

    // ============================================================== geometry
    output logic [31:0]                   o_prf_entries_o,
    output logic [31:0]                   o_tag_w_o,
    output logic [31:0]                   o_gen_w_o,
    output logic [31:0]                   o_arch_regs_o,
    output logic [31:0]                   o_rob_entries_o,
    output logic [31:0]                   o_rob_index_w_o,
    output logic [31:0]                   o_id_w_o,
    output logic [31:0]                   o_ckpt_depth_w_o,
    output logic [31:0]                   o_rdq_depth_w_o,
    output logic [31:0]                   o_cred_w_o,
    output logic [31:0]                   o_epoch_w_o,
    output logic [31:0]                   o_cnt_w_o,
    output logic [31:0]                   o_map_w_o
);

  // ==================================================================== state
  // The speculative rename layer this module owns. Reset to the architectural
  // reset state, which is a *defined* state and not merely a convenient one:
  // arch reg i mapped to tag i at generation 0.
  logic [REC_TAG_W-1:0] spec_map [REC_ARCH];
  logic [REC_GEN_W-1:0] spec_gen [REC_ARCH];
  logic [REC_TAG_W-1:0] cmt_map  [REC_ARCH];
  logic [REC_GEN_W-1:0] cmt_gen  [REC_ARCH];

  // Free set: a set, not a stack. A stack's checkpoint is a pointer, and that
  // pointer is not a correct snapshot -- a tag freed while the stack head sits
  // below the checkpoint overwrites a slot a later pop will read, and restoring
  // the pointer hands back a stack whose contents are no longer the saved ones.
  // Restoring a set is exact. That is the whole reason this is a bitmap, and it
  // is the reason the free list is not checkpointed per branch but replayed.
  logic [REC_ENTRIES-1:0] free_bits;
  logic [REC_ENTRIES-1:0] gen_valid;
  logic [REC_ENTRIES-1:0] wb_done;

  // Data arrays: never reset. One reset bit per tag lives in gen_valid.
  logic [REC_GEN_W-1:0]  gen  [REC_ENTRIES];
  logic [REC_TAG_W-1:0]  j_tag [REC_ROB];
  logic                  j_prev_valid [REC_ROB];

  // Control state.
  logic [REC_TAG_W-1:0]     alloc_ptr;   // the rotation point of the allocation scan
  logic [REC_ROB_IDX_W-1:0] tail;        // the speculative ROB tail
  logic [REC_CNT_W-1:0]     j_len;       // the undo window
  logic [REC_EPOCH_W-1:0]   epoch;
  logic                     j_overflow;
  logic [31:0]              journal_entries;
  logic [31:0]              restores;
  logic [31:0]              ckpt_taken;
  logic [31:0]              credit_returns;
  logic [31:0]              rsp_stale_total;

  // The ROB generation of the last accepted allocation, and whether there has
  // been one. These exist so the *order* of allocations is checked here rather
  // than assumed by the redirect arbiter downstream: the arbiter selects the
  // oldest candidate by unsigned minimum over this counter, and that minimum
  // only means "oldest" for as long as the counter increases.
  logic [REC_ID_W-1:0] last_alloc_gen;
  logic                alloc_seen;

  // The checkpoint stack. A stack, not a queue, and that is forced: a redirect
  // restores to *its own* checkpoint, and the checkpoints above it are the
  // younger branches it kills. Restoring to the wrong one is the defect named
  // in the header, and a stack makes the wrong one the obvious bug to write.
  logic [CKPT_DEPTH-1:0]          ck_valid;
  logic [REC_CNT_W-1:0]           ck_jmark    [CKPT_DEPTH];
  logic [REC_ID_W-1:0]            ck_gen      [CKPT_DEPTH];
  logic [REC_ROB_IDX_W-1:0]       ck_tail     [CKPT_DEPTH];
  logic [REC_TAG_W-1:0]           ck_alloc_ptr[CKPT_DEPTH];
  logic [REC_EPOCH_W-1:0]         ck_epoch    [CKPT_DEPTH];
  logic [REC_ARCH*(REC_MAP_W)-1:0] ck_spec     [CKPT_DEPTH];
  // The checkpointing instruction's own destination, and the generation state it
  // replaced. A branch that writes a link register allocates a tag in the *same*
  // cycle it takes its checkpoint, and that allocation is deliberately not
  // journalled -- the checkpoint precedes the branch, so the branch's own
  // allocation is not part of the window the undo walks. Without recording it
  // here, a mispredicting call leaks one tag: the allocation happened, the undo
  // never sees it, and the free count drifts down by one per such branch until
  // the machine stalls with an empty free list it believes is full.
  //
  // It follows that a restore has to apply the inverse step to the recorded
  // destination of **every checkpoint it consumes**, not only its own: a squash
  // kills the younger branches too, and each of those owns a destination that no
  // other undo covers. The rule is stated once -- the journal's own inverse step,
  // the same one every journalled allocation gets -- so there is one undo rule in
  // the design rather than two.
  logic                   ck_tag_valid  [CKPT_DEPTH];
  logic [REC_TAG_W-1:0]   ck_tag        [CKPT_DEPTH];
  logic                   ck_tag_prev_valid [CKPT_DEPTH];
  logic [REC_GEN_W-1:0]   ck_tag_prev_gen   [CKPT_DEPTH];

  logic [REC_CKPT_W-1:0]          ck_ptr;     // next push slot
  logic [REC_CKPT_W:0]            ck_depth;

  // Credit reservation table. A table of *slots*, not a counter: a counter
  // cannot distinguish a second delivery of a credit from the first, and
  // "exactly once" is precisely the property that needs the distinction.
  logic [CRED_ENTRIES-1:0]  cred_busy;
  // One bit per reservation: "the redirect that owns this slot's instruction
  // has been taken, so its result must be discarded and its credit returned".
  // It is *latched* at the redirect rather than recomputed from the epoch at
  // the response, because the epoch alone cannot tell an instruction the
  // redirect squashed from one that was already live before it -- and killing
  // the latter is a stated failure, not a conservative choice (see the credit
  // return path below).
  logic [CRED_ENTRIES-1:0]  cred_cancel;
  logic [REC_EPOCH_W-1:0]   cred_epoch [CRED_ENTRIES];
  logic [REC_ID_W-1:0]      cred_gen   [CRED_ENTRIES];

  // The pending-redirect queue. A ring of slots with an explicit valid vector:
  // redirects are consumed out of the middle (the oldest is taken, the rest are
  // killed and the whole queue is emptied), so there is no compaction to do and
  // no ordering to maintain between survivors.
  logic [RDQ_DEPTH-1:0]  rq_valid;
  logic [REC_ID_W-1:0]   rq_gen  [RDQ_DEPTH];
  logic [REC_XLEN-1:0]   rq_pc   [RDQ_DEPTH];
  logic                  rq_fault[RDQ_DEPTH];
  logic [REC_RDQ_W:0]    rq_count;

  // ============================================================== small helpers

  // The reset value of the free set: every tag except the architectural reset
  // mappings, which are *owned* by those mappings and therefore not free. A
  // free list reset to all-ones hands tag 5 to the first instruction that writes
  // any register, giving one physical register two live owners and turning a
  // later writeback into a silent corruption of an architectural value.
  function automatic logic [REC_ENTRIES-1:0] init_owned_mask();
    logic [REC_ENTRIES-1:0] mask;
    mask = {REC_ENTRIES{1'b1}};
    for (int unsigned a = 0; a < REC_ARCH; a++) begin
      mask[a] = 1'b0;
    end
    return mask;
  endfunction

  function automatic logic [REC_FCNT_W-1:0] count_free(input logic [REC_ENTRIES-1:0] bits);
    logic [REC_FCNT_W-1:0] acc;
    acc = {REC_FCNT_W{1'b0}};
    for (int unsigned b = 0; b < REC_ENTRIES; b++) begin
      acc = acc + REC_FCNT_W'(bits[b]);
    end
    return acc;
  endfunction

  function automatic logic [REC_CRED_W:0] count_busy(input logic [CRED_ENTRIES-1:0] bits);
    logic [REC_CRED_W:0] acc;
    acc = {(REC_CRED_W + 1){1'b0}};
    for (int unsigned b = 0; b < CRED_ENTRIES; b++) begin
      acc = acc + (REC_CRED_W + 1)'(bits[b]);
    end
    return acc;
  endfunction

  function automatic logic [REC_ROB_IDX_W-1:0] next_rob_idx(input logic [REC_ROB_IDX_W-1:0] idx);
    next_rob_idx = (idx == REC_ROB_IDX_W'(REC_ROB - 1)) ? {REC_ROB_IDX_W{1'b0}}
                                                         : (idx + REC_ROB_IDX_W'(1));
  endfunction

  // ====================================================== allocation machinery

  // A rotating priority scan over the free set, doubling the mask so the wrap is
  // an index computation rather than a second loop. The rotation point plus the
  // offset is at most 2*ENTRIES-2, so one conditional subtract is the whole
  // modulus and no general division is elaborated. Both operands are widened
  // first, so neither the add nor the compare can silently truncate a tag.
  logic [2*REC_ENTRIES-1:0] free_dup;
  logic                    scan_found;
  logic [REC_TAG_W-1:0]    scan_tag;

  assign free_dup = {free_bits, free_bits};

  always_comb begin
    scan_found = 1'b0;
    scan_tag   = {REC_TAG_W{1'b0}};
    for (int unsigned i = 0; i < REC_ENTRIES; i++) begin
      logic [REC_TAG_W:0] off;
      logic [REC_TAG_W-1:0] idx;
      off = (REC_TAG_W+1)'(alloc_ptr) + (REC_TAG_W+1)'(i);
      idx = REC_TAG_W'((off >= (REC_TAG_W+1)'(REC_ENTRIES))
                       ? (off - (REC_TAG_W+1)'(REC_ENTRIES)) : off);
      if (!scan_found && free_dup[off]) begin
        scan_found = 1'b1;
        scan_tag   = idx;
      end
    end
  end

  // The monotonicity check on the ROB generation, stated here next to the
  // allocation it constrains. `last_alloc_gen` and `alloc_seen` are declared
  // with the rest of the control state.
  assign alloc_gen_regress = alloc_new_valid && alloc_seen &&
                             (alloc_rob_gen <= last_alloc_gen);

  // A producer that reuses a reserved credit id would be given a second
  // reservation, and the two would be returned separately: the double-credit
  // defect arriving from upstream rather than from inside this unit. It is
  // reported, and the reservation still proceeds, because refusing it would
  // deadlock a producer with no other way to make progress.
  // Declared here rather than beside its second use because the request-side and
  // the response-side range checks must agree, and one rule stated once cannot
  // drift from itself.
  //
  // A credit id is a `REC_CRED_W`-bit field, so a CRED_ENTRIES that does not
  // fill that space can be named by a value that is not a slot. The shipping
  // geometry is a power of two and the check folds to a constant, which is
  // exactly the dead logic -Wall exists to report, so the comparison is
  // elaborated only where it can actually truncate. This is mosaic_rob's
  // IDX_NEVER_TRUNCATES rule applied to the credit table.
  localparam bit CRED_NEVER_TRUNCATES = (CRED_ENTRIES == (1 << REC_CRED_W));

  logic cred_req_id_in_range;
  assign cred_req_id_in_range = CRED_NEVER_TRUNCATES ? 1'b1
                                        : (cred_req_id < REC_CRED_W'(CRED_ENTRIES));
  assign cred_req_conflict    = cred_req_valid && cred_req_id_in_range &&
                               cred_busy[cred_req_id];

  logic has_free;
  assign has_free = |free_bits;

  // ------------------------------------------------------------------ refusal
  // Each refusal has its own report. "Refused" with no reason would leave the
  // caller unable to tell back-pressure (retry later) from a squash (do not
  // retry) from a write to x0 (nothing to retry at all) -- and the first two
  // need opposite behaviour from everything upstream. A squash wins: the
  // instruction is discarded regardless of whether a tag was available.

  // The mapping a commit supersedes, and the one a commit installs. A repeated
  // commit of the same mapping is a no-op rather than a free of the mapping the
  // commit just installed.
  logic commit_supersedes;
  assign commit_supersedes = commit_valid && (commit_rd != 5'd0) &&
                             ((cmt_map[commit_rd] != commit_tag) ||
                              (cmt_gen[commit_rd] != commit_gen));

  // ================================================================== writeback
  logic wb_in_range;
  logic free_in_range;
  assign wb_in_range   = (wb_tag < REC_TAG_W'(REC_ENTRIES));
  assign free_in_range = (free_tag < REC_TAG_W'(REC_ENTRIES));

  // "Stale" means: not the tag's current owner. That covers a recycled tag with
  // an old generation, a tag freed since, a tag never allocated since reset, and
  // a tag outside the array. The generation check is what makes a *rollback* on
  // a squash reject an escaping writeback by exactly the same rule as a merely
  // late one: there is no second mechanism, and therefore no second thing to get
  // wrong.
  assign wb_stale     = wb_valid && (!wb_in_range || !gen_valid[wb_tag] ||
                                     free_bits[wb_tag] || (wb_gen != gen[wb_tag]));
  assign wb_duplicate = wb_valid && !wb_stale && wb_done[wb_tag];
  assign wb_accepted  = wb_valid && !wb_stale && !wb_done[wb_tag];

  assign free_stale    = free_valid && (!free_in_range || !gen_valid[free_tag] ||
                                        (free_gen != gen[free_tag]));
  assign free_double   = free_valid && !free_stale && free_bits[free_tag];
  assign free_accepted = free_valid && !free_stale && !free_bits[free_tag];

  // ==================================================================== commit
  assign commit_x0_dropped = commit_valid && (commit_rd == 5'd0);
  assign commit_accepted   = commit_valid && (commit_rd != 5'd0);

  // ===================================================== redirect arbitration
  //
  // Candidates are the queue and this cycle's two resolve ports, and the oldest
  // is the minimum generation over all of them together.
  // Source 0 is the first queue slot; the two resolve ports are RDQ_DEPTH and
  // RDQ_DEPTH + 1. There is no `SRC_Q` localparam because the queue slots are
  // addressed by their own loop index in the copy below, and a constant that is
  // only ever equal to 0 is a name rather than a fact.
  localparam int unsigned NSRC  = RDQ_DEPTH + 2;  // ... plus the two resolve ports
  localparam int unsigned SRC_0 = RDQ_DEPTH;
  localparam int unsigned SRC_1 = RDQ_DEPTH + 1;

  logic                  src_live  [NSRC];
  logic [REC_ID_W-1:0]   src_gen   [NSRC];
  logic [REC_XLEN-1:0]   src_pc    [NSRC];
  logic                  src_fault [NSRC];

  always_comb begin
    for (int unsigned q = 0; q < RDQ_DEPTH; q++) begin
      src_live  [q] = rq_valid[q];
      src_gen   [q] = rq_gen[q];
      src_pc    [q] = rq_pc[q];
      src_fault [q] = rq_fault[q];
    end
    src_live  [SRC_0] = redirect0_valid;
    src_gen   [SRC_0] = redirect0_rob_gen;
    src_pc    [SRC_0] = redirect0_pc;
    src_fault [SRC_0] = redirect0_is_fault;
    src_live  [SRC_1] = redirect1_valid;
    src_gen   [SRC_1] = redirect1_rob_gen;
    src_pc    [SRC_1] = redirect1_pc;
    src_fault [SRC_1] = redirect1_is_fault;
  end

  // A redirect is *stale* if it names a generation with no live checkpoint: its
  // branch was squashed by an earlier recovery, so it describes a path that no
  // longer exists. Taken-but-dead is worse than not-taken, so this is a
  // separate report rather than a silent drop.
  logic src_has_ckpt [NSRC];
  logic src_stale   [NSRC];
  always_comb begin
    for (int unsigned s = 0; s < NSRC; s++) begin
      src_has_ckpt[s] = 1'b0;
      for (int unsigned c = 0; c < CKPT_DEPTH; c++) begin
        if (ck_valid[c] && (ck_gen[c] == src_gen[s])) begin
          src_has_ckpt[s] = 1'b1;
        end
      end
      src_stale[s] = src_live[s] && !src_has_ckpt[s];
    end
  end

  // The oldest live, non-stale candidate. Ties between *different* instructions
  // cannot happen -- generations are unique per allocation -- but two sources
  // naming the same generation is a duplicate producer, and the lower source
  // index wins so the behaviour is deterministic rather than order-dependent.
  logic                pick_found;
  logic [REC_PICK_W-1:0] pick_src;
  logic [REC_ID_W-1:0] pick_gen;
  logic [REC_XLEN-1:0] pick_pc;
  logic                pick_fault;
  logic [REC_CKPT_W-1:0] pick_ck;

  always_comb begin
    pick_found = 1'b0;
    pick_src   = {REC_PICK_W{1'b0}};
    pick_gen   = {REC_ID_W{1'b0}};
    pick_pc    = {REC_XLEN{1'b0}};
    pick_fault = 1'b0;
    pick_ck    = {REC_CKPT_W{1'b0}};
    for (int unsigned s = 0; s < NSRC; s++) begin
      if (src_live[s] && !src_stale[s] &&
          (!pick_found || (src_gen[s] < pick_gen))) begin
        pick_found = 1'b1;
        pick_src   = REC_PICK_W'(s);
        pick_gen   = src_gen[s];
        pick_pc    = src_pc[s];
        pick_fault = src_fault[s];
        pick_ck    = {REC_CKPT_W{1'b0}};
        for (int unsigned c = 0; c < CKPT_DEPTH; c++) begin
          if (ck_valid[c] && (ck_gen[c] == src_gen[s])) begin
            pick_ck = REC_CKPT_W'(c);
          end
        end
      end
    end
  end

  // ------------------------------------------------------------- the restore
  // The restore point is the taken redirect's own checkpoint.
  //
  // MUTANT 1 (WRONG_CHECKPOINT) restores to the *deepest* live checkpoint
  // instead. That is restoring to an earlier point than the redirect asked for:
  // it undoes allocations belonging to instructions older than the redirecting
  // branch, so a still-surviving older instruction finds its destination
  // register handed to somebody else. It is the single most damaging defect
  // this unit can have, which is why it is the first mutant.
  logic [REC_CKPT_W-1:0] restore_ck;
  // The mutant scans upward and keeps the highest valid index, which is the
  // deepest live checkpoint and therefore the same wrong answer a downward scan
  // would produce. It is written that way because a downward scan over an
  // `int unsigned` cannot elaborate at all: `c >= 0` is constant-true, so the
  // loop is infinite and Verilator refuses the build -- a mutant that does not
  // elaborate proves nothing, and this one did not, in the file as delivered.
`ifdef MOSAIC_RECOVERY_MUTANT_WRONG_CHECKPOINT
  always_comb begin
    restore_ck = {REC_CKPT_W{1'b0}};
    for (int unsigned c = 0; c < CKPT_DEPTH; c++) begin
      if (ck_valid[c]) begin
        restore_ck = REC_CKPT_W'(c);
      end
    end
  end
`else
  assign restore_ck = pick_ck;
`endif

  assign redirect_taken         = pick_found;
  assign redirect_taken_gen     = pick_gen;
  assign redirect_taken_pc      = pick_pc;
  assign redirect_taken_is_fault = pick_fault;
  // The reduction over `src_stale` is written out rather than written
  // `assign redirect_stale = |src_stale;`, because `src_stale` is an unpacked
  // array and the reduction operator is not defined on one.
  logic any_stale;
  always_comb begin
    any_stale = 1'b0;
    for (int unsigned s = 0; s < NSRC; s++) begin
      any_stale = any_stale || src_stale[s];
    end
  end
  assign redirect_stale          = any_stale;
  assign o_redirect_src         = pick_src;
  assign o_restore_ckpt         = restore_ck;

  // Every other live candidate is younger than the one taken, so the same squash
  // kills it. Reported, so a killed redirect is never silently lost.
  logic [REC_RDQ_W:0] killed_count;
  always_comb begin
    killed_count = {(REC_RDQ_W + 1){1'b0}};
    for (int unsigned s = 0; s < NSRC; s++) begin
      if (src_live[s] && !src_stale[s] && !(pick_found && (REC_PICK_W'(s) == pick_src))) begin
        killed_count = killed_count + (REC_RDQ_W + 1)'(1);
      end
    end
  end
  assign redirect_killed = killed_count;

  // The action outputs are pure functions of the decision above, so there is no
  // cycle in which a redirect is reported taken and no squash happens, and none
  // in which a squash happens without a redirect to justify it.
  //
  // `retire_block` is high in exactly one situation: a cycle in which a redirect
  // is taken *and* the ROB is retiring. A retire in any other cycle is permanent
  // and needs no block at all: its free is permanent, and its journal entry sits
  // below the oldest live checkpoint's mark, so no restore can ever undo it.
  // Blocking unconditionally would stop retirement whenever a branch was in
  // flight, which is most of the time.
  assign squash                  = pick_found;
  assign retire_block            = pick_found && rob_retire;
  assign rob_flush_valid         = pick_found;
  assign o_rob_flush_from_valid  = pick_found;
  assign o_rob_flush_from        = ck_tail[restore_ck];

  // The restore decision, factored out because two independent things depend on
  // it -- the map/tail/rotation-point next state, and nothing else. Declared
  // here rather than inside the next-state block so the `ifdef` that makes the
  // mutant is a single pair of assignments rather than a nested conditional.
  //
  // MUTANT 4 (NO_TAIL_RESTORE) makes the tail and the rotation point stay where
  // they are while the map and the free set are restored correctly. That is the
  // subtlest failure in this set: the free list is *right* and the next
  // allocation is still wrong, and nothing looks wrong until the restored
  // machine hands a physical register to an instruction that should have got a
  // different one.
  logic tail_do_restore;
  logic alloc_ptr_do_restore;
`ifdef MOSAIC_RECOVERY_MUTANT_NO_TAIL_RESTORE
  assign tail_do_restore      = 1'b0;
  assign alloc_ptr_do_restore = 1'b0;
`else
  assign tail_do_restore      = pick_found && ck_valid[restore_ck];
  assign alloc_ptr_do_restore = tail_do_restore;
`endif

  // ================================================== journal / checkpoint math
  // How many entries the restore has to undo: everything above the checkpoint's
  // mark. Clamped to the journal size, and the clamp is exactly the overflow
  // this unit refuses to create, so the clamp is unreachable in the shipping
  // build and is present only so a hostile `j_len` cannot index out of range.
  logic [REC_CNT_W-1:0] undo_n;
  // The window base lives at the journal index width from the start: it is
  // always a live journal position below REC_ROB, so the count width's top
  // bit would be dead and -Wall would (rightly) say so.
  logic [REC_JIDX_W-1:0] undo_base;
  always_comb begin
    undo_n = {REC_CNT_W{1'b0}};
    undo_base = {REC_JIDX_W{1'b0}};
    if (pick_found && ck_valid[restore_ck] && (j_len > ck_jmark[restore_ck])) begin
      undo_n = j_len - ck_jmark[restore_ck];
      // The window starts at the checkpoint's own mark, not at entry zero: the
      // journal is shared by every live checkpoint, and entries below the mark
      // belong to instructions older than the branch. Indexing from zero undoes
      // those too, freeing tags the checkpoint never owned and handing the
      // restored machine a free set larger than the one recorded. The mark is a
      // live journal position below REC_ROB, so it fits the index width exactly.
      undo_base = REC_JIDX_W'(ck_jmark[restore_ck]);
    end
    if (undo_n > REC_CNT_W'(REC_ROB)) begin
      undo_n = REC_CNT_W'(REC_ROB);
    end
  end

  // How many entries the next-state block actually applies. Equal to `undo_n` in
  // every build except the mutant that removes the free-set restore, and kept a
  // separate signal so that mutant is a one-line change rather than a second
  // driver on `undo_n`.
  logic [REC_CNT_W-1:0] undo_apply;
  assign undo_apply = undo_n;
  // The first journal entry the restore undoes: the window is
  // [undo_apply_base, undo_apply_base + undo_apply), never [0, undo_apply).
  logic [REC_JIDX_W-1:0] undo_apply_base;
  // Already at the journal index width: direct assignment, no cast.
  assign undo_apply_base = undo_base;
  // Whether the undo's effects are applied at all.
  //
  // MUTANT 5 (NO_FREE_RESTORE) is a negative control: the undo window is walked
  // and the journal is consumed, but the free set, the generations and the
  // generation-valid bits keep whatever the squashed instructions left behind.
  // The speculative map still goes back to the checkpoint's copy, so the restored
  // machine's free list no longer matches its mappings and the free count drifts
  // by one tag per squashed instruction, while the journal still reports the
  // restore as having happened.
  //
  // It is an enable on the effects rather than a zero count because a count that
  // is a constant makes the loop's own comparison constant, the build stops on
  // that warning, and a mutant that does not elaborate proves nothing -- which is
  // how the previous version of this one behaved.
  logic undo_effects;
`ifdef MOSAIC_RECOVERY_MUTANT_NO_FREE_RESTORE
  assign undo_effects = 1'b0;
`else
  assign undo_effects = 1'b1;
`endif

  // A checkpoint is refused when the stack is full, or in the cycle a redirect
  // is taken -- the squash owns that cycle, and pushing a checkpoint for a
  // branch that is being squashed would leave a checkpoint for a dead branch.
  logic ckpt_room;
  logic ckpt_push;
  assign ckpt_room    = (ck_depth < (REC_CKPT_W + 1)'(CKPT_DEPTH));
  assign ckpt_push    = ckpt_valid && ckpt_room && !pick_found;
  assign ckpt_accepted = ckpt_push;
  assign ckpt_refused  = ckpt_valid && !ckpt_push;

  // The undo bound, computed here rather than with the journal bookkeeping below
  // because it is a *refusal*: an allocation that would push the window past its
  // bound is not accepted, and the window is never wrapped. The decision has to
  // be made where acceptance is decided, or `alloc_journal_full` reports a
  // refusal that did not happen while the journal write below wraps its index and
  // overwrites a live entry -- a restore then returns a state the machine never
  // passed through, while reporting success.
  logic alloc_wants_tag;
  logic journal_at_bound;
  logic alloc_journal_refused;
  assign alloc_wants_tag = alloc_valid && (alloc_rd != 5'd0);
  assign journal_at_bound = (j_len >= REC_CNT_W'(REC_ROB));
  // A write to x0 allocates nothing and journals nothing, so the bound cannot
  // apply to it; a checkpointing instruction's own destination is recorded in the
  // checkpoint rather than the journal, so the bound does not apply to it either;
  // and the squash owns a squash cycle regardless.
  assign alloc_journal_refused = alloc_wants_tag && journal_at_bound &&
                                 !ckpt_push && !squash;
  assign alloc_squashed  = alloc_valid && squash;
  assign alloc_is_x0     = alloc_valid && (alloc_rd == 5'd0) && !squash;
  assign alloc_exhausted = alloc_wants_tag && !has_free && !squash;
  assign alloc_journal_full = alloc_journal_refused;
  assign journal_overflow   = alloc_journal_refused;
  // MUTANT 7 (ACCEPT_AT_BOUND) drops the refusal from acceptance while keeping the
  // report, which is the defect this block was written to remove: the allocation
  // is taken, `alloc_journal_full` says it was refused, and the journal write
  // below lands on the index its own width wrapped -- overwriting a live entry, so
  // a later restore returns a state the machine never passed through while
  // reporting success. The case catches it on the allocation's own report, in the
  // cycle it happens.
`ifdef MOSAIC_RECOVERY_MUTANT_ACCEPT_AT_BOUND
  assign alloc_accepted  = alloc_valid && !squash && (has_free || (alloc_rd == 5'd0));
`else
  assign alloc_accepted  = alloc_valid && !squash && !alloc_journal_refused &&
                           (has_free || (alloc_rd == 5'd0));
`endif
  assign alloc_new_valid = alloc_accepted && (alloc_rd != 5'd0);

  assign alloc_new_tag = scan_tag;
  assign alloc_new_gen = gen_valid[scan_tag] ? (gen[scan_tag] + REC_GEN_W'(1))
                                             : {REC_GEN_W{1'b0}};
  assign alloc_old_valid = alloc_new_valid;
  assign alloc_old_tag   = spec_map[alloc_rd];
  assign alloc_old_gen   = spec_gen[alloc_rd];

  // Does this allocation need a journal entry? It does unless it is the
  // checkpointing instruction: the checkpoint is taken at that instruction and
  // the restore restores to *before* it, so its own allocation must not be in
  // the window. That is also why a branch that writes a link register does not
  // leak its tag on its own mispredict.
  logic alloc_needs_journal;
  assign alloc_needs_journal = alloc_new_valid && !ckpt_push;

  // The undo bound itself is computed with the allocation refusals, because
  // reaching it exactly is legal -- the steady state for a full queue -- and
  // exceeding it is a refusal there, not merely a report. `alloc_journal_full`
  // and `journal_overflow` are the two reports of that one decision, and
  // `alloc_new_valid` is false while it holds, so nothing is journalled and the
  // window can never exceed its array.
  logic [REC_CNT_W-1:0] j_len_q;
  logic                  j_overflow_q;

  always_comb begin
    j_len_q       = j_len;
    j_overflow_q  = j_overflow;
    j_overflow_q  = j_overflow_q | journal_overflow;

    if (alloc_needs_journal) begin
      j_len_q = j_len + REC_CNT_W'(1);
    end

    if (pick_found && ck_valid[restore_ck]) begin
      j_len_q = ck_jmark[restore_ck];
    end
  end

  // ================================================== credit return path
  // The response-side half of the range rule declared with the request side.
  logic rsp_slot_in_range;
  logic rsp_slot_busy;
  generate
    if (CRED_NEVER_TRUNCATES) begin : g_cred_never_truncates
      assign rsp_slot_in_range = 1'b1;
    end else begin : g_cred_may_truncate
      assign rsp_slot_in_range = (rsp_id < REC_CRED_W'(CRED_ENTRIES));
    end
  endgenerate
  assign rsp_slot_busy    = rsp_slot_in_range && cred_busy[rsp_id];

  // Classification asks two different questions, and the answers come from two
  // different fields. Conflating them is the defect this section exists to
  // avoid, so they are named separately.
  //
  //   * Age -- "did the redirect squash this instruction?" A redirect kills
  //     every instruction younger than the branch it resolves, and how old a
  //     reservation is is exactly what `cred_gen`, the owner generation
  //     recorded at grant, says. A reservation whose owner generation is at or
  //     above the redirect's is killed; one whose owner is *older* survives,
  //     and its result must still land. The epoch cannot answer this: an older
  //     load or DIV that is still live across a younger branch's mispredict
  //     also carries a pre-redirect epoch, and dropping it is a named failure,
  //     not a conservative choice. `docs/implementation-plan.md` §1.3 requires
  //     the PRF/result epoch check to respect "the older instruction that is
  //     still live across a redirect", and the I-018 card's blocking rule lists
  //     "kill every old-epoch instruction" as the way to deadlock the older ROB
  //     head -- the instruction whose result is dropped never completes.
  //     `cred_cancel` is that comparison latched at the redirect; a redirect
  //     taken in *this* cycle is applied combinationally by `rsp_killed_by_now`
  //     so a response arriving with the redirect is classified against the same
  //     boundary, and the slot's own clear below makes the credit return
  //     exactly once either way.
  //   * Identity -- "is this response the one the slot is waiting for?" The
  //     carried epoch must equal the epoch recorded when the slot was reserved.
  //     That is the (slot, epoch) identity `config/contracts/interfaces.json`
  //     requires: without it a response for a recycled slot is decoded as the
  //     live reservation's result. A busy slot whose epoch does not match is
  //     therefore *not* a live response for this reservation, and it is not a
  //     stale one either -- no credit is owed, because the credit was returned
  //     when the reservation it names was acknowledged.
  //
  // MUTANT 2 (APPLY_STALE_RSP) treats every response for a reserved slot as
  // live, so a squashed instruction's result is written into the restored
  // machine -- the ABA hazard interfaces.json names, made concrete.
  // MUTANT 6 (EPOCH_ONLY_RSP) is the classification this section replaced: any
  // response from before the redirect is dropped, so an older instruction whose
  // result is still in flight is killed.
  logic rsp_identity_matches;
  logic rsp_killed_by_now;
  logic rsp_cancelled;
  assign rsp_identity_matches = rsp_slot_busy &&
                                (rsp_epoch == cred_epoch[rsp_id]);
  assign rsp_killed_by_now    = rsp_slot_busy && pick_found &&
                                ck_valid[restore_ck] &&
                                (cred_gen[rsp_id] >= pick_gen);
  assign rsp_cancelled        = rsp_slot_busy &&
                                (cred_cancel[rsp_id] || rsp_killed_by_now);

`ifdef MOSAIC_RECOVERY_MUTANT_APPLY_STALE_RSP
  assign rsp_accepted      = rsp_valid && rsp_slot_busy;
  assign rsp_dropped_stale = 1'b0;
  assign rsp_dropped_dup   = rsp_valid && rsp_slot_in_range && !rsp_slot_busy;
`elsif MOSAIC_RECOVERY_MUTANT_EPOCH_ONLY_RSP
  assign rsp_accepted      = rsp_valid && rsp_slot_busy && (rsp_epoch == epoch);
  assign rsp_dropped_stale = rsp_valid && rsp_slot_busy && (rsp_epoch != epoch);
  assign rsp_dropped_dup   = rsp_valid && rsp_slot_in_range && !rsp_slot_busy;
`else
  assign rsp_accepted      = rsp_valid && rsp_identity_matches && !rsp_cancelled;
  assign rsp_dropped_stale = rsp_valid && rsp_cancelled;
  // A free slot is a duplicate delivery of an already-acknowledged credit. A
  // busy slot whose epoch is not the one it was reserved with is the same thing
  // one recycling later: the response names a reservation that is gone, and
  // returning a credit for it would inflate the free-credit count.
  assign rsp_dropped_dup   = rsp_valid && rsp_slot_in_range &&
                             (!rsp_slot_busy ||
                              (!rsp_cancelled && !rsp_identity_matches));
`endif
  assign rsp_dropped_orphan = rsp_valid && !rsp_slot_in_range;

  // MUTANT 3 (DOUBLE_CREDIT) returns the credit on the drop *and* on the
  // duplicate, so a producer that delivers twice inflates the free-credit count
  // and the fabric eventually over-issues: two producers, one destination.
`ifdef MOSAIC_RECOVERY_MUTANT_DOUBLE_CREDIT
  assign credit_return = rsp_dropped_stale || rsp_dropped_dup;
`else
  assign credit_return = rsp_dropped_stale;
`endif

  // ------------------------------------------------------------ credit table
  logic cred_free_found;
  logic [REC_CRED_W-1:0] cred_free_id;
  always_comb begin
    cred_free_found = 1'b0;
    cred_free_id   = {REC_CRED_W{1'b0}};
    for (int unsigned c = 0; c < CRED_ENTRIES; c++) begin
      if (!cred_free_found && !cred_busy[c]) begin
        cred_free_found = 1'b1;
        cred_free_id   = REC_CRED_W'(c);
      end
    end
  end

  assign cred_req_full = ~cred_free_found;
  assign cred_req_ok   = cred_req_valid && cred_free_found;
  assign credits_outstanding = count_busy(cred_busy);

  // A redirect does **not** return the credits it cancels. The contract in
  // config/contracts/interfaces.json returns a credit when the cancel is
  // acknowledged, and the acknowledgement is the arrival of the response that is
  // now stale. Returning it at the redirect as well would be the mirror image of
  // a credit leak and exactly as fatal.
  //
  // What the redirect *does* do is mark the slots it owns as cancelled, and the
  // kill set is an age boundary rather than the whole table: the reservation's
  // owner generation is compared with the redirecting branch's, and only
  // reservations at or above it are cancelled. Applying the boundary to the
  // table *as it stands after this cycle's grant* also covers the reservation
  // the grant is making in the redirect cycle itself -- a credit taken for a
  // uop the same cycle's squash is discarding -- without a second rule.
  logic [CRED_ENTRIES-1:0]   cred_busy_q;
  logic [CRED_ENTRIES-1:0]   cred_cancel_q;
  logic [REC_EPOCH_W-1:0]    cred_epoch_q [CRED_ENTRIES];
  logic [REC_ID_W-1:0]       cred_gen_q   [CRED_ENTRIES];
  always_comb begin
    cred_busy_q   = cred_busy;
    cred_cancel_q = cred_cancel;
    for (int unsigned c = 0; c < CRED_ENTRIES; c++) begin
      cred_epoch_q[c] = cred_epoch[c];
      cred_gen_q  [c] = cred_gen[c];
    end
    if (cred_req_ok) begin
      cred_busy_q   [cred_free_id] = 1'b1;
      cred_cancel_q [cred_free_id] = 1'b0;
      cred_epoch_q  [cred_free_id] = epoch;
      cred_gen_q    [cred_free_id] = cred_req_rob_gen;
    end
    if (pick_found && ck_valid[restore_ck]) begin
      for (int unsigned c = 0; c < CRED_ENTRIES; c++) begin
        if (cred_busy_q[c] && (cred_gen_q[c] >= pick_gen)) begin
          cred_cancel_q[c] = 1'b1;
        end
      end
    end
    // Any response frees its slot, whatever the outcome. That is what makes a
    // second delivery of the same slot find nothing left to return -- and why
    // the cancel is cleared here too: a redirect and the response it kills in
    // one cycle must return the credit exactly once, and a later copy of that
    // response then finds a free slot.
    if (rsp_valid && rsp_slot_in_range) begin
      cred_busy_q  [rsp_id] = 1'b0;
      cred_cancel_q[rsp_id] = 1'b0;
    end
  end

  // =============================================== next state: masks and journal
  logic [REC_ENTRIES-1:0] free_q;
  logic [REC_ENTRIES-1:0] genv_q;
  logic [REC_ENTRIES-1:0] wbd_q;
  logic [REC_GEN_W-1:0]   gen_q [REC_ENTRIES];

  always_comb begin
    free_q = free_bits;
    genv_q = gen_valid;
    wbd_q  = wb_done;
    for (int unsigned e = 0; e < REC_ENTRIES; e++) begin
      gen_q[e] = gen[e];
    end

    // 1. Allocation: take a tag out of the set, advance its generation, and give
    //    it a fresh "not yet written" flag.
    if (alloc_new_valid) begin
      free_q[scan_tag] = 1'b0;
      gen_q[scan_tag]  = gen_valid[scan_tag] ? (gen[scan_tag] + REC_GEN_W'(1))
                                              : {REC_GEN_W{1'b0}};
      genv_q[scan_tag] = 1'b1;
      wbd_q[scan_tag]  = 1'b0;
    end

    // 2. An explicit release, and the mapping a commit supersedes, both put a
    //    tag back. Neither touches the generation: it counts allocations, so a
    //    tag that goes free and is handed out again comes back with a larger
    //    number and an old writeback cannot match it.
    if (free_accepted) begin
      free_q[free_tag] = 1'b1;
    end
    if (commit_supersedes) begin
      free_q[cmt_map[commit_rd]] = 1'b1;
    end

    // 3. The undo, oldest entry first so the newest is applied last. Allocation
    //    is refused in a squash cycle, so this can never race with step 1. The
    //    free set is a set, so its order does not matter; the generation is not,
    //    so its undo is the exact inverse of the allocation's step.
    for (int unsigned k = 0; k < REC_ROB; k++) begin
      if (REC_CNT_W'(k) < undo_apply) begin
        // Offset by the checkpoint's mark: entry k of the undo window is
        // journal entry (undo_apply_base + k), not entry k. Both are counts
        // below REC_ROB, so the sum stays inside the journal array; it is
        // computed at the journal index width -- the width every other journal
        // access uses -- so no bit is unused and none is silently truncated.
        automatic logic [REC_JIDX_W-1:0] undo_idx =
            undo_apply_base + REC_JIDX_W'(k);
        if (undo_effects) begin
          free_q[j_tag[undo_idx]] = 1'b1;
          gen_q[j_tag[undo_idx]]  =
              j_prev_valid[undo_idx]
                ? (gen[j_tag[undo_idx]] - REC_GEN_W'(1))
                : {REC_GEN_W{1'b0}};
          genv_q[j_tag[undo_idx]] = j_prev_valid[undo_idx];
        end
      end
    end

    // 3b. Every consumed checkpoint owns a destination of its own: the restored
    //     branch, and every younger branch whose checkpoint the same squash
    //     kills. None of those allocations is in the journal -- a checkpoint push
    //     deliberately does not journal its own instruction -- so each is undone
    //     here, by the journal's own inverse rule, or a mispredicting call leaks
    //     one tag per *killed* branch in addition to the one it leaks per
    //     mispredict. The leak surfaces dozens of instructions later as spurious
    //     exhaustion with nothing pointing back at the branches that caused it.
    //     The tags are distinct allocations, so the order of this loop cannot
    //     matter, exactly as the free set's set semantics guarantee for the undo
    //     window itself.
    // MUTANT 8 (RESTORE_SELF_ONLY) undoes only the *restored* checkpoint's own
    // destination, which is the leak this loop was widened to close: a squash
    // consumes every younger branch's checkpoint too, and each of those owns a
    // destination that no journal entry covers, so those tags stay allocated
    // forever while their mappings are rewound away. The case catches it in the
    // nested phase's free-mask comparison against the checkpoint.
`ifdef MOSAIC_RECOVERY_MUTANT_RESTORE_SELF_ONLY
    if (tail_do_restore && ck_tag_valid[restore_ck]) begin
      free_q[ck_tag[restore_ck]] = 1'b1;
      gen_q[ck_tag[restore_ck]]  = ck_tag_prev_valid[restore_ck]
                                       ? ck_tag_prev_gen[restore_ck]
                                       : {REC_GEN_W{1'b0}};
      genv_q[ck_tag[restore_ck]] = ck_tag_prev_valid[restore_ck];
    end
`else
    if (tail_do_restore) begin
      for (int unsigned c = 0; c < CKPT_DEPTH; c++) begin
        if (ck_valid[c] && (c >= restore_ck) && ck_tag_valid[c]) begin
          free_q[ck_tag[c]] = 1'b1;
          gen_q[ck_tag[c]]  = ck_tag_prev_valid[c]
                                  ? ck_tag_prev_gen[c]
                                  : {REC_GEN_W{1'b0}};
          genv_q[ck_tag[c]] = ck_tag_prev_valid[c];
        end
      end
    end
`endif

    // 4. The owner that made it back writes.
    if (wb_accepted) begin
      wbd_q[wb_tag] = 1'b1;
    end
  end

  // =============================================== next state: the maps and tail
  logic [REC_TAG_W-1:0]     spec_map_q [REC_ARCH];
  logic [REC_GEN_W-1:0]     spec_gen_q [REC_ARCH];
  logic [REC_TAG_W-1:0]     cmt_map_q  [REC_ARCH];
  logic [REC_GEN_W-1:0]     cmt_gen_q  [REC_ARCH];
  logic [REC_ROB_IDX_W-1:0] tail_q;
  logic [REC_TAG_W-1:0]     alloc_ptr_q;
  logic [REC_EPOCH_W-1:0]   epoch_q;

  // The checkpoint's speculative RAT copy, taken out of the array first so no
  // part-select is chained onto a variable index.
  logic [REC_ARCH*(REC_MAP_W)-1:0] restore_spec;
  assign restore_spec = ck_spec[restore_ck];

  // The speculative map as it stands before this cycle's allocation: what a
  // checkpoint taken this cycle records.
  logic [REC_ARCH*(REC_MAP_W)-1:0] live_spec;
  always_comb begin
    for (int unsigned a = 0; a < REC_ARCH; a++) begin
      live_spec[a*REC_MAP_W +: REC_MAP_W] = {spec_gen[a], spec_map[a]};
    end
  end

  // The next state, written once. The order of the three cases below is the
  // whole priority rule and it is stated as three separate `if`s on disjoint
  // conditions rather than as one nested chain, so the reader can see that a
  // restore, a commit and an allocation cannot all be live at once:
  //
  //   * `tail_do_restore` and `commit_accepted` CAN both be high. That is the
  //     interesting case, and the rule is that the commit lands first: a commit
  //     is permanent, so the restore must read the post-commit committed map.
  //     Restoring over it would resurrect the mapping the commit just replaced,
  //     and the next dispatch would allocate against a dead mapping.
  //   * `tail_do_restore` and `alloc_new_valid` cannot both be high: the squash
  //     owns the cycle and allocation is refused in a squash cycle.
  always_comb begin
    for (int unsigned a = 0; a < REC_ARCH; a++) begin
      spec_map_q[a] = spec_map[a];
      spec_gen_q[a] = spec_gen[a];
      cmt_map_q[a]  = cmt_map[a];
      cmt_gen_q[a]  = cmt_gen[a];
    end
    tail_q      = tail;
    alloc_ptr_q = alloc_ptr;
    epoch_q     = epoch;

    if (commit_accepted) begin
      cmt_map_q[commit_rd] = commit_tag;
      cmt_gen_q[commit_rd] = commit_gen;
    end

    if (tail_do_restore) begin
      for (int unsigned a = 0; a < REC_ARCH; a++) begin
        spec_map_q[a] = restore_spec[a*REC_MAP_W +: REC_TAG_W];
        spec_gen_q[a] = restore_spec[a*REC_MAP_W + REC_TAG_W +: REC_GEN_W];
      end
      tail_q  = ck_tail[restore_ck];
      epoch_q = epoch + REC_EPOCH_W'(1);
    end else if (alloc_new_valid) begin
      spec_map_q[alloc_rd] = scan_tag;
      spec_gen_q[alloc_rd] = alloc_new_gen;
    end

    // The rotation point follows the last allocation, so a fully free list hands
    // tags out in ascending order and wraps after ENTRIES allocations. It is
    // restored from the checkpoint, not derived, because "the free set is right
    // and the scan starts somewhere else" is exactly the near-miss this module
    // exists to rule out.
    if (alloc_ptr_do_restore) begin
      alloc_ptr_q = ck_alloc_ptr[restore_ck];
    end else if (alloc_new_valid) begin
      alloc_ptr_q = (scan_tag == REC_TAG_W'(REC_ENTRIES - 1)) ? {REC_TAG_W{1'b0}}
                                                             : (scan_tag + REC_TAG_W'(1));
    end

    // The tail follows allocation, and only allocation. A retire does not move
    // the tail; a squash moves it to the checkpoint's value, not to zero and not
    // to the head.
    if (!tail_do_restore && alloc_new_valid) begin
      tail_q = next_rob_idx(tail);
    end
  end

  // ================================================== next state: the checkpoint
  logic [CKPT_DEPTH-1:0]             ck_valid_q;
  logic [REC_CKPT_W-1:0]             ck_ptr_q;
  logic [REC_CKPT_W:0]               ck_depth_q;
  logic [REC_CNT_W-1:0]              ck_jmark_q    [CKPT_DEPTH];
  logic [REC_ID_W-1:0]               ck_gen_q      [CKPT_DEPTH];
  logic [REC_ROB_IDX_W-1:0]          ck_tail_q     [CKPT_DEPTH];
  logic [REC_TAG_W-1:0]              ck_alloc_ptr_q[CKPT_DEPTH];
  logic [REC_EPOCH_W-1:0]            ck_epoch_q    [CKPT_DEPTH];
  logic [REC_ARCH*(REC_MAP_W)-1:0]   ck_spec_q     [CKPT_DEPTH];
  logic                              ck_tag_valid_q     [CKPT_DEPTH];
  logic [REC_TAG_W-1:0]              ck_tag_q           [CKPT_DEPTH];
  logic                              ck_tag_prev_valid_q [CKPT_DEPTH];
  logic [REC_GEN_W-1:0]              ck_tag_prev_gen_q   [CKPT_DEPTH];

  // A restore consumes its own checkpoint and every checkpoint above it: those
  // are the branches the squash kills. A checkpoint *below* it belongs to an
  // older, still-surviving instruction and stays.
  logic [REC_CKPT_W:0] killed_ckpts;
  always_comb begin
    killed_ckpts = {(REC_CKPT_W + 1){1'b0}};
    for (int unsigned c = 0; c < CKPT_DEPTH; c++) begin
      if (ck_valid[c] && (c >= restore_ck)) begin
        killed_ckpts = killed_ckpts + (REC_CKPT_W + 1)'(1);
      end
    end
  end

  always_comb begin
    ck_valid_q = ck_valid;
    ck_ptr_q   = ck_ptr;
    ck_depth_q = ck_depth;
    for (int unsigned c = 0; c < CKPT_DEPTH; c++) begin
      ck_jmark_q    [c] = ck_jmark[c];
      ck_gen_q      [c] = ck_gen[c];
      ck_tail_q     [c] = ck_tail[c];
      ck_alloc_ptr_q[c] = ck_alloc_ptr[c];
      ck_epoch_q    [c] = ck_epoch[c];
      ck_spec_q     [c] = ck_spec[c];
      ck_tag_valid_q[c] = ck_tag_valid[c];
      ck_tag_q      [c] = ck_tag[c];
      ck_tag_prev_valid_q[c] = ck_tag_prev_valid[c];
      ck_tag_prev_gen_q[c] = ck_tag_prev_gen[c];
    end

    if (pick_found) begin
      for (int unsigned c = 0; c < CKPT_DEPTH; c++) begin
        if (ck_valid[c] && (c >= restore_ck)) begin
          ck_valid_q[c] = 1'b0;
        end
      end
      ck_depth_q = ck_depth - killed_ckpts;
      ck_ptr_q   = restore_ck;
    end

    // A checkpoint records the speculative state as it stands *before* this
    // cycle's allocation, so a branch's checkpoint precedes the branch and the
    // restore puts the machine back before the branch dispatched.
    if (ckpt_push) begin
      ck_valid_q[ck_ptr]     = 1'b1;
      ck_jmark_q[ck_ptr]     = j_len;
      ck_gen_q  [ck_ptr]     = ckpt_rob_gen;
      ck_tail_q [ck_ptr]     = tail;
      ck_alloc_ptr_q[ck_ptr] = alloc_ptr;
      ck_epoch_q[ck_ptr]     = epoch;
      ck_spec_q [ck_ptr]     = live_spec;
      // The branch's own destination, captured from the *pre-edge* generation
      // state: that is what the restore's inverse step has to put back, and the
      // allocation which changed it happens in this same cycle. Reading it after
      // the allocation would store the already-incremented generation and the
      // restore would step it back to the wrong value -- the same one-cycle
      // ordering trap the shadow fell into on the other five fields.
      ck_tag_valid_q[ck_ptr]     = alloc_new_valid;
      ck_tag_q      [ck_ptr]     = scan_tag;
      ck_tag_prev_valid_q[ck_ptr] = gen_valid[scan_tag];
      ck_tag_prev_gen_q[ck_ptr]   = gen[scan_tag];
      ck_ptr_q   = (ck_ptr == REC_CKPT_W'(CKPT_DEPTH - 1)) ? {REC_CKPT_W{1'b0}}
                                                          : (ck_ptr + REC_CKPT_W'(1));
      ck_depth_q = ck_depth + (REC_CKPT_W + 1)'(1);
    end
  end

  // ============================================================== next state: queue
  //
  // Two rules, and both matter:
  //
  //   * A taken redirect **empties** the queue. Every other pending redirect is
  //     younger than the one taken, so the same squash kills it; keeping one
  //     would mean taking a redirect for a branch that no longer exists on the
  //     next cycle.
  //   * Otherwise the survivors stay where they are and this cycle's resolve
  //     ports that are neither taken nor stale are **appended**, port 0 before
  //     port 1, into the lowest free slots. Appending rather than shifting
  //     matters because a take empties the queue from the middle: there is no
  //     order among survivors to preserve, so there is nothing to compact.
  //
  // The free-slot search is written as one pass over a running occupancy mask
  // rather than as two independent searches, because two independent searches
  // would return the *same* slot for both ports and one redirect would overwrite
  // the other. That is the bug the single pass exists to prevent.
  logic [RDQ_DEPTH-1:0]           rq_valid_q;
  logic [REC_ID_W-1:0]            rq_gen_q   [RDQ_DEPTH];
  logic [REC_XLEN-1:0]            rq_pc_q    [RDQ_DEPTH];
  logic                          rq_fault_q [RDQ_DEPTH];
  logic [REC_RDQ_W:0]             rq_count_q;

  // A taken redirect kills every other live candidate, so in a take cycle there
  // is nothing to append and the whole queue goes. Otherwise the two resolve
  // ports that are neither taken nor stale are appended in port order.
  always_comb begin
    // Procedural locals, not signals: each is consumed once by the pass below,
    // and a driven signal that is also assigned inside a combinational block
    // would be two drivers.
    logic app0;
    logic app1;
    app0 = !pick_found && src_live[SRC_0] && !src_stale[SRC_0];
    app1 = !pick_found && src_live[SRC_1] && !src_stale[SRC_1];

    for (int unsigned q = 0; q < RDQ_DEPTH; q++) begin
      rq_gen_q  [q] = rq_gen[q];
      rq_pc_q   [q] = rq_pc[q];
      rq_fault_q[q] = rq_fault[q];
    end

    if (pick_found) begin
      rq_valid_q = {RDQ_DEPTH{1'b0}};
      rq_count_q = {(REC_RDQ_W + 1){1'b0}};
    end else begin
      rq_valid_q      = rq_valid;
      rq_count_q      = rq_count;
      // A running mask of the slots already handed out this cycle. It starts as
      // the queue's own occupancy, so the first free slot is the first slot the
      // queue has not used, and the second port cannot be given the same slot.
      for (int unsigned q = 0; q < RDQ_DEPTH; q++) begin
        if (rq_valid_q[q]) begin
          continue;
        end
        if (app0) begin
          rq_valid_q[q]   = 1'b1;
          rq_gen_q  [q]   = src_gen[SRC_0];
          rq_pc_q   [q]   = src_pc[SRC_0];
          rq_fault_q[q]   = src_fault[SRC_0];
          rq_count_q      = rq_count_q + (REC_RDQ_W + 1)'(1);
          app0            = 1'b0;
        end else if (app1) begin
          rq_valid_q[q]   = 1'b1;
          rq_gen_q  [q]   = src_gen[SRC_1];
          rq_pc_q   [q]   = src_pc[SRC_1];
          rq_fault_q[q]   = src_fault[SRC_1];
          rq_count_q      = rq_count_q + (REC_RDQ_W + 1)'(1);
          app1            = 1'b0;
        end
      end
    end
  end

  // ==================================================================== registers
  always_ff @(posedge clk) begin
    if (rst) begin
      // The two maps are reset because their reset state is architecturally
      // defined: leaving them to power-up contents makes the first read of every
      // architectural register undefined. They are 32-entry files with two
      // combinational read ports, so they are flip-flops, not an inferred RAM,
      // and the "no resettable arrays" RAM rule does not apply to them.
      for (int unsigned a = 0; a < REC_ARCH; a++) begin
        spec_map[a] <= REC_TAG_W'(a);
        spec_gen[a] <= {REC_GEN_W{1'b0}};
        cmt_map[a]  <= REC_TAG_W'(a);
        cmt_gen[a]  <= {REC_GEN_W{1'b0}};
      end
      // Tags 0..ARCH-1 are owned by the architectural reset mappings, not free.
      free_bits  <= init_owned_mask();
      gen_valid  <= {REC_ENTRIES{1'b0}};
      wb_done    <= {REC_ENTRIES{1'b0}};
      // The rotation point starts at the first allocatable tag, so the machine's
      // first allocation takes the lowest free tag rather than scanning past the
      // reserved range.
      alloc_ptr  <= REC_TAG_W'(REC_ARCH);
      tail       <= {REC_ROB_IDX_W{1'b0}};
      j_len      <= {REC_CNT_W{1'b0}};
      epoch      <= {REC_EPOCH_W{1'b0}};
      j_overflow <= 1'b0;
      // The allocation-order tracker is control state, and it is the validity
      // bit -- not the value -- that a reset has to clear, the same rule the
      // generation table follows. Leaving `alloc_seen` set across a reset makes
      // the first allocation afterwards compare its generation against the
      // previous run's and report `alloc_gen_regress` on a producer that did
      // nothing wrong; the report is the only thing this state drives, so a
      // stale bit is a status output that lies for the first ROB_ENTRIES
      // allocations after every reset.
      last_alloc_gen <= {REC_ID_W{1'b0}};
      alloc_seen     <= 1'b0;
      journal_entries <= 32'd0;
      restores         <= 32'd0;
      ckpt_taken       <= 32'd0;
      credit_returns   <= 32'd0;
      rsp_stale_total  <= 32'd0;
      ck_valid  <= {CKPT_DEPTH{1'b0}};
      ck_ptr    <= {REC_CKPT_W{1'b0}};
      ck_depth  <= {(REC_CKPT_W + 1){1'b0}};
      rq_valid  <= {RDQ_DEPTH{1'b0}};
      rq_count  <= {(REC_RDQ_W + 1){1'b0}};
      cred_busy   <= {CRED_ENTRIES{1'b0}};
      cred_cancel <= {CRED_ENTRIES{1'b0}};
    end else begin
      for (int unsigned a = 0; a < REC_ARCH; a++) begin
        spec_map[a] <= spec_map_q[a];
        spec_gen[a] <= spec_gen_q[a];
        cmt_map[a]  <= cmt_map_q[a];
        cmt_gen[a]  <= cmt_gen_q[a];
      end
      free_bits <= free_q;
      gen_valid <= genv_q;
      wb_done   <= wbd_q;
      for (int unsigned e = 0; e < REC_ENTRIES; e++) begin
        gen[e] <= gen_q[e];
      end

      alloc_ptr <= alloc_ptr_q;
      tail      <= tail_q;
      j_len     <= j_len_q;
      j_overflow <= j_overflow_q;
      epoch     <= epoch_q;

      // One journal entry per allocation, holding the state that allocation
      // replaced. The previous generation itself is not stored: the undo is the
      // exact inverse of the allocation's increment, so one bit is the whole
      // undo state.
      if (alloc_needs_journal) begin
        j_tag[REC_JIDX_W'(j_len)] <= scan_tag;
        j_prev_valid[REC_JIDX_W'(j_len)] <= gen_valid[scan_tag];
      end

      if (alloc_needs_journal) begin
        journal_entries <= journal_entries + 32'd1;
      end

      // The generation tracker follows the last *accepted* allocation, not the
      // last offered one: a refused allocation consumes no age, and folding it in
      // would make the next accepted generation look like a regression.
      if (alloc_new_valid) begin
        last_alloc_gen <= alloc_rob_gen;
        alloc_seen     <= 1'b1;
      end
      if (pick_found) begin
        restores <= restores + 32'd1;
      end
      if (credit_return) begin
        credit_returns <= credit_returns + 32'd1;
      end
      if (rsp_dropped_stale) begin
        rsp_stale_total <= rsp_stale_total + 32'd1;
      end

      ck_valid <= ck_valid_q;
      ck_ptr   <= ck_ptr_q;
      ck_depth <= ck_depth_q;
      for (int unsigned c = 0; c < CKPT_DEPTH; c++) begin
        ck_jmark    [c] <= ck_jmark_q[c];
        ck_gen      [c] <= ck_gen_q[c];
        ck_tail     [c] <= ck_tail_q[c];
        ck_alloc_ptr[c] <= ck_alloc_ptr_q[c];
        ck_epoch    [c] <= ck_epoch_q[c];
        ck_spec     [c] <= ck_spec_q[c];
        ck_tag_valid[c] <= ck_tag_valid_q[c];
        ck_tag      [c] <= ck_tag_q[c];
        ck_tag_prev_valid[c] <= ck_tag_prev_valid_q[c];
        ck_tag_prev_gen[c] <= ck_tag_prev_gen_q[c];
      end
      if (ckpt_push) begin
        ckpt_taken <= ckpt_taken + 32'd1;
      end

      rq_valid <= rq_valid_q;
      rq_count <= rq_count_q;
      for (int unsigned q = 0; q < RDQ_DEPTH; q++) begin
        rq_gen  [q] <= rq_gen_q[q];
        rq_pc   [q] <= rq_pc_q[q];
        rq_fault[q] <= rq_fault_q[q];
      end

      cred_busy   <= cred_busy_q;
      cred_cancel <= cred_cancel_q;
      for (int unsigned c = 0; c < CRED_ENTRIES; c++) begin
        cred_epoch[c] <= cred_epoch_q[c];
        cred_gen  [c] <= cred_gen_q[c];
      end
    end
  end

  // ================================================================ reporting
  assign free_count        = count_free(free_bits);
  assign o_journal_entries = journal_entries;
  assign o_restores        = restores;
  assign o_ckpt_taken      = ckpt_taken;
  assign o_credit_returns  = credit_returns;
  assign o_rsp_stale       = rsp_stale_total;
  assign o_ckpt_depth      = ck_depth;
  assign o_rdq_depth       = rq_count;
  assign o_epoch           = epoch;

  assign dbg_free_mask   = free_bits;
  assign dbg_gen_valid   = gen_valid;
  assign dbg_wb_done     = wb_done;
  assign dbg_tail        = tail;
  assign dbg_alloc_ptr   = alloc_ptr;
  assign dbg_j_len       = j_len;
  assign dbg_ckpt_valid  = ck_valid;
  assign dbg_inexact     = j_overflow;

  for (genvar g = 0; g < REC_ENTRIES; g++) begin : g_dbg_tag_gen
    assign dbg_tag_gen[g*REC_GEN_W +: REC_GEN_W] = gen[g];
  end

  for (genvar m = 0; m < REC_ARCH; m++) begin : g_dbg_map
    assign dbg_spec_map[m*REC_MAP_W +: REC_MAP_W] = {spec_gen[m], spec_map[m]};
    assign dbg_cmt_map [m*REC_MAP_W +: REC_MAP_W] = {cmt_gen[m], cmt_map[m]};
  end

  for (genvar c = 0; c < CKPT_DEPTH; c++) begin : g_dbg_ckpt
    assign dbg_ckpt_jmark    [c*REC_CNT_W +: REC_CNT_W]           = ck_jmark[c];
    assign dbg_ckpt_gen      [c*REC_ID_W +: REC_ID_W]             = ck_gen[c];
    assign dbg_ckpt_tail     [c*REC_ROB_IDX_W +: REC_ROB_IDX_W]   = ck_tail[c];
    assign dbg_ckpt_alloc_ptr[c*REC_TAG_W +: REC_TAG_W]           = ck_alloc_ptr[c];
    assign dbg_ckpt_epoch    [c*REC_EPOCH_W +: REC_EPOCH_W]       = ck_epoch[c];
  end

  // ================================================================== geometry
  // Read back from the elaborated parameters rather than from the derivation
  // above, so the driver can check the two against each other.
  assign o_prf_entries_o  = 32'(REC_ENTRIES);
  assign o_tag_w_o        = 32'(REC_TAG_W);
  assign o_gen_w_o        = 32'(REC_GEN_W);
  assign o_arch_regs_o    = 32'(REC_ARCH);
  assign o_rob_entries_o  = 32'(REC_ROB);
  assign o_rob_index_w_o  = 32'(REC_ROB_IDX_W);
  assign o_id_w_o         = 32'(REC_ID_W);
  assign o_ckpt_depth_w_o = 32'(REC_CKPT_W);
  assign o_rdq_depth_w_o  = 32'(REC_RDQ_W);
  assign o_cred_w_o       = 32'(REC_CRED_W);
  assign o_epoch_w_o      = 32'(REC_EPOCH_W);
  assign o_cnt_w_o        = 32'(REC_CNT_W);
  assign o_map_w_o        = 32'(REC_MAP_W);

endmodule : mosaic_recovery

`resetall
`default_nettype wire
