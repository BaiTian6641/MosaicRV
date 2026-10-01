// ============================================================================
// mosaic_redirect_arb -- outstanding-redirect arbitration (work package I-023).
//
// Each cluster that resolves a branch raises a request {pc, identity, taken}.
// Several clusters can resolve a branch in the same cycle, and only one of them
// may act. This module decides which, and it is the *only* place that drives a
// redirect.
//
// ------------------------------------------------------------------- old规则
//
// The rule is **oldest wins, and only at the head**:
//
//   * Among live requests, the one whose ROB slot is oldest -- smallest distance
//     behind the ROB head -- is the one that acts. A younger branch behind an
//     *older taken* branch is on the wrong path by construction, so its request
//     is dropped (and counted) rather than left to redirect the machine twice.
//   * A request may only act in the cycle its own macro is the ROB **head** and
//     that head is being retired (`head_retire`). Until then the request waits;
//     older work keeps retiring, which is required -- a held redirect must not
//     stall retirement of the instructions in front of it.
//
//     "The head" is *both* entries the ROB retires in the cycle: lane 0
//     (`head_*`) and the entry immediately behind it (`head1_*`). The core
//     retires two per cycle, and a resolved branch lands in either lane
//     depending on how the older instructions completed; a request one slot
//     behind the head that was not accepted as acting there would never become
//     the lane-0 head, because the ROB can retire both entries in the same
//     cycle and be empty afterwards. That is a deadlock, not a wait, and it is
//     what CASE=core.corpus_branch found. Accepting lane 1 keeps the rule's
//     purpose: by the end of that cycle every older instruction has committed,
//     either in this cycle or an earlier one. A consumer with a single retire
//     lane ties `head1_*` low and gets the original rule exactly.
//
// ------------------------------------------------------ why the head gate
//
// `mosaic_rename`'s squash restores the *speculative map from the committed
// map* and rolls the free list back through its undo journal. That is a
// drain-to-commit-boundary restore, not a full checkpoint: it is exact only when
// the redirecting branch is the oldest uncommitted instruction. A squash taken
// while older instructions are still in flight would destroy their speculative
// mappings, and fetch redirects to the branch's target, so those instructions
// would never be re-executed and their architectural writes would be lost
// silently. The head gate is what makes the squash correct: when the branch
// reaches the head, its commit is the newest committed state, and everything
// behind it is exactly what the flush discards.
//
// This is the conservative in-top arbiter. I-018's recovery controller replaces
// it once the duplicated rename state is in place (the decision is recorded in
// results/PROGRESS.md); this arbiter is a real, tested component and not a
// placeholder -- the fabric case drives it directly as well as through the core.
//
// --------------------------------------------------------------- liveness
//
// The ROB is a hole-free circular buffer: the live slots are exactly the
// `o_occupied` entries starting at the head. So a request is live iff its
// slot's distance behind the head is below the occupancy; a request whose slot
// has since retired, or whose index now belongs to a different generation, is
// dead, is acknowledged without acting (so the requester can clear it) and is
// counted. No shadow copy of ROB validity is needed for any of this.
//
// ------------------------------------------------------------- negative controls
//
//   MOSAIC_REDIRECT_MUTANT_YOUNGEST_WINS  the youngest live request acts, so a
//                                         wrong-path branch redirects.
//   MOSAIC_REDIRECT_MUTANT_NO_HEAD_WAIT   the oldest live request acts at once,
//                                         without waiting for its macro to reach
//                                         the head; the squash then destroys
//                                         older unretired work.
// ============================================================================

`default_nettype none
`resetall

/* verilator lint_off UNUSEDPARAM */
`include "mosaic_cfg_pkg.svh"
/* verilator lint_on UNUSEDPARAM */
/* verilator lint_off UNUSEDSIGNAL */
`include "mosaic_id_pkg.svh"
/* verilator lint_on UNUSEDSIGNAL */

localparam int unsigned RDA_N       = mosaic_cfg_pkg::MOSAIC_CLUSTERS;
localparam int unsigned RDA_XLEN    = mosaic_cfg_pkg::MOSAIC_XLEN;
localparam int unsigned RDA_IDX_W   = mosaic_id_pkg::MOSAIC_ID_W_ROB_INDEX;
localparam int unsigned RDA_RGEN_W  = mosaic_id_pkg::MOSAIC_ID_W_ROB_GEN;
localparam int unsigned RDA_OCC_W   = $clog2(mosaic_cfg_pkg::MOSAIC_ROB_ENTRIES + 1);
localparam int unsigned RDA_WIN_W   = (RDA_N <= 1) ? 1 : $clog2(RDA_N);

module mosaic_redirect_arb (
    input  logic                       clk,
    input  logic                       rst,

    // ------------------------------------------------------ one port per cluster
    input  logic [RDA_N-1:0]           req_valid,
    input  logic [RDA_N-1:0][RDA_XLEN-1:0]  req_pc,
    input  logic [RDA_N-1:0][RDA_IDX_W-1:0] req_rob_index,
    input  logic [RDA_N-1:0][RDA_RGEN_W-1:0] req_rob_gen,
    input  logic [RDA_N-1:0]           req_taken,
    output logic [RDA_N-1:0]           req_ack,     // consumed: acted on or dropped

    // ------------------------------------------------- the system/trap request
    // This is the third thing that can redirect the front end: a trap entry or
    // an MRET. It is not a cluster branch, and it is deliberately *not* another
    // element of the `req_*` array, because its two rules differ:
    //
    //   * it is always at the ROB head by construction -- the core raises it
    //     only for the entry it is looking at this cycle -- so it is always the
    //     oldest live request and always wins;
    //   * `sys_req_act_now` lets it act in the cycle its entry is the head even
    //     when that entry is *not* retiring. A trapping instruction never
    //     retires (its exception is architecturally final and the flush drops
    //     it), so the branch rule's `head_retire` gate would make a trap
    //     impossible to take. An MRET *does* retire, so it leaves act_now low
    //     and uses the ordinary head-retire gate.
    //
    // `sys_req_taken` is implicitly 1: a trap or an MRET always redirects.
    input  logic                       sys_req_valid,
    input  logic [RDA_XLEN-1:0]        sys_req_pc,
    input  logic [RDA_IDX_W-1:0]       sys_req_rob_index,
    input  logic [RDA_RGEN_W-1:0]      sys_req_rob_gen,
    input  logic                       sys_req_act_now,
    // The system request acted (this cycle) and the redirect it caused is now
    // registered. The core uses `o_sys_act`/`o_sys_redirect` to route the trap
    // path's recovery (a full restore) instead of a branch's checkpoint.
    output logic                       o_sys_act,
    output logic                       o_sys_redirect,

    // ------------------------------------------------- the ROB's head view
    // The ROB retires two entries per cycle, and a resolved branch can be in
    // either lane. "The head" for the purpose of this rule is therefore *both*
    // entries that are leaving this cycle: lane 0 (`off == 0`) and the entry
    // immediately behind it (`off == 1`). By the end of that cycle every
    // instruction older than the request has committed either way, which is
    // exactly what the squash the rule exists to make safe requires. A core
    // with one retire lane ties the second view low and gets the single-lane
    // rule unchanged -- which is what the standalone directed test does.
    input  logic                       head_valid,
    input  logic [RDA_IDX_W-1:0]       head_index,
    input  logic [RDA_RGEN_W-1:0]      head_gen,
    input  logic [RDA_OCC_W-1:0]       head_occupied,
    input  logic                       head_retire, // lane 0 leaves this cycle
    input  logic                       head1_valid,
    input  logic [RDA_IDX_W-1:0]       head1_index,
    input  logic [RDA_RGEN_W-1:0]      head1_gen,
    input  logic                       head1_retire, // lane 1 leaves this cycle

    // ------------------------------------------------------------- the action
    // A one-cycle pulse in the cycle *after* the redirecting head retired, so
    // the flush it triggers cannot also erase the instruction that caused it.
    output logic                       redirect_valid,
    output logic [RDA_XLEN-1:0]        redirect_pc,
    // The decision, in the cycle it is made: a resolution was acted on, and
    // whether it was taken. The core uses these to release its branch barrier --
    // a not-taken resolution is accounted for without any flush, so the frontend
    // may resume at once.
    output logic                       o_act_valid,
    output logic                       o_act_taken,

    // ---------------------------------------------------------------- counts
    output logic [31:0]                o_req_ctr,
    output logic [31:0]                o_act_ctr,
    output logic [31:0]                o_drop_ctr,    // wrong-path losers
    output logic [31:0]                o_dead_ctr,    // requests for a dead slot
    output logic [31:0]                o_wait_ctr,    // cycles a live request waited
    output logic [31:0]                o_nothing_ctr  // not-taken resolutions acted
);

  // ------------------------------------------------------------- classification
  logic [RDA_N-1:0][RDA_IDX_W-1:0] off;      // distance behind the head
  logic [RDA_N-1:0]                cand;     // live: inside the occupied window
  logic [RDA_N-1:0]                dead;

  always_comb begin
    for (int unsigned i = 0; i < RDA_N; i++) begin
      off[i]  = req_rob_index[i] - head_index;      // wrapping, mod 2**IDX_W
      // Live slots are head .. head+occupied-1, so "inside the window" is the
      // whole liveness test. An empty ROB (occupancy 0) makes every request
      // dead, which is what a redirect that just flushed everything leaves.
      cand[i] = req_valid[i] && head_valid &&
                (RDA_OCC_W'(off[i]) < head_occupied);
      // A live entry at a head index must carry that head's generation; a
      // different generation at the same index means the slot has been recycled
      // and the request belongs to an instruction that no longer exists.
      dead[i] = req_valid[i] && !cand[i];
      if (req_valid[i] && cand[i] && (off[i] == {RDA_IDX_W{1'b0}}) &&
          (req_rob_gen[i] != head_gen)) begin
        dead[i] = 1'b1;
        cand[i] = 1'b0;
      end
      // The same test for the second retiring lane. A request one behind the
      // head whose generation disagrees with that entry's is not the
      // instruction in it.
      if (req_valid[i] && cand[i] && (off[i] == RDA_IDX_W'(1)) && head1_valid &&
          (req_rob_gen[i] != head1_gen)) begin
        dead[i] = 1'b1;
        cand[i] = 1'b0;
      end
    end
  end

  // ----------------------------------------------------- the system candidate
  // The same liveness test and the same generation test, evaluated once for the
  // one extra port. Because the core only ever raises this request for the
  // entry it is looking at, the offset is 0 in every legal use; the offset
  // comparison below is what makes "oldest wins" true for it too, rather than
  // an assumption this module would be silently relying on.
  logic                 sys_cand;
  logic                 sys_dead;
  logic [RDA_IDX_W-1:0] sys_off;

  always_comb begin
    sys_off  = sys_req_rob_index - head_index;
    sys_cand = sys_req_valid && head_valid &&
               (RDA_OCC_W'(sys_off) < head_occupied);
    sys_dead = sys_req_valid && !sys_cand;
    if (sys_req_valid && sys_cand && (sys_off == {RDA_IDX_W{1'b0}}) &&
        (sys_req_rob_gen != head_gen)) begin
      sys_dead = 1'b1;
      sys_cand = 1'b0;
    end
    if (sys_req_valid && sys_cand && (sys_off == RDA_IDX_W'(1)) && head1_valid &&
        (sys_req_rob_gen != head1_gen)) begin
      sys_dead = 1'b1;
      sys_cand = 1'b0;
    end
  end

  // ----------------------------------------------------------------- winner
  // Oldest = closest behind the head = smallest offset.
  logic        win_found;
  logic [RDA_WIN_W-1:0] win_i;

  always_comb begin
    win_found = 1'b0;
    win_i     = 1'b0;
    for (int unsigned i = 0; i < RDA_N; i++) begin
`ifdef MOSAIC_REDIRECT_MUTANT_YOUNGEST_WINS
      // NEGATIVE CONTROL 1: the youngest live request is the winner, so a
      // wrong-path branch can redirect the machine.
      if (cand[i] && (!win_found || (off[i] > off[win_i]))) begin
`else
      if (cand[i] && (!win_found || (off[i] < off[win_i]))) begin
`endif
        win_found = 1'b1;
        win_i     = RDA_WIN_W'(i);
      end
    end
  end

  // ---------------------------------------------------------------- the action
  logic act_sys;
  logic at_head0, at_head1;
  logic sys_at_head0, sys_at_head1;
  logic sys_win;
  logic act_win;

  // The system request is older than the cluster winner whenever its offset is
  // smaller. It is at the head in every legal use, so this is normally decisive
  // on the first comparison; a cluster request can never tie with it, because
  // two requests cannot name the same entry with different meanings.
  always_comb begin
    sys_win = sys_cand && (!win_found || (sys_off < off[win_i]));
  end

  // The system request acting. `act_now` is the trap's licence to act on a head
  // that is not retiring; an MRET uses the ordinary gate.
  always_comb begin
    sys_at_head0 = head_valid && (sys_req_rob_index == head_index) &&
                   (sys_req_rob_gen == head_gen);
    sys_at_head1 = head1_valid && (sys_req_rob_index == head1_index) &&
                   (sys_req_rob_gen == head1_gen);
    act_sys = sys_win &&
              ((sys_req_act_now && (sys_at_head0 || sys_at_head1)) ||
               (sys_at_head0 && head_retire) ||
               (sys_at_head1 && head1_retire));
  end

`ifdef MOSAIC_REDIRECT_MUTANT_NO_HEAD_WAIT
  // NEGATIVE CONTROL 2: the winner acts immediately, without waiting for its
  // macro to reach the head. Everything older than it in the ROB is still in
  // flight and is about to be squashed by a restore that does not cover it.
  assign at_head0 = 1'b0;
  assign at_head1 = 1'b0;
  assign act_win  = act_sys || (win_found && !sys_win);
`else
  // Identity equality, not an offset: the request names the instruction, and it
  // may act when the entry being retired this cycle *is* that instruction -- in
  // lane 0 or in lane 1. Both are "the head" for the purpose the rule enforces;
  // see the head-view note in the port list.
  assign at_head0 = head_valid && head_retire &&
                    (req_rob_index[win_i] == head_index) &&
                    (req_rob_gen[win_i] == head_gen);
  assign at_head1 = head1_valid && head1_retire &&
                    (req_rob_index[win_i] == head1_index) &&
                    (req_rob_gen[win_i] == head1_gen);
  assign act_win  = act_sys || (win_found && !sys_win && (at_head0 || at_head1));
`endif

  // Whether the request that supplied this cycle's redirect had its transfer
  // taken: a system request always redirects; a cluster request only when its
  // branch resolved taken.
  logic win_taken;

  assign win_taken = act_sys ? 1'b1 : req_taken[win_i];

  assign o_sys_act = act_sys;

  always_comb begin
    for (int unsigned i = 0; i < RDA_N; i++) begin
      req_ack[i] = 1'b0;
      if (dead[i]) begin
        req_ack[i] = 1'b1;
      end
      if (act_win && !act_sys && (RDA_WIN_W'(i) == win_i)) begin
        req_ack[i] = 1'b1;
      end
      // A system redirect is a trap entry or an MRET: it discards everything at
      // and above the head, so every outstanding cluster request is consumed --
      // it belongs to an instruction that no longer exists.
      if (act_sys && req_valid[i]) begin
        req_ack[i] = 1'b1;
      end
      // Young taken losers are wrong-path: the winner's redirect flushes them.
      // A not-taken winner drops nothing -- a younger taken branch behind a
      // not-taken one is on the correct path and must keep its request.
      if (act_win && !act_sys && req_taken[win_i] && (RDA_WIN_W'(i) != win_i) &&
          req_taken[i]) begin
        req_ack[i] = 1'b1;
      end
    end
  end

  // ------------------------------------------------------- registered pulse
  logic              redirect_q;
  logic [RDA_XLEN-1:0] redirect_pc_q;
  logic              sys_redirect_q;

  assign redirect_valid = redirect_q;
  assign redirect_pc    = redirect_pc_q;
  assign o_sys_redirect = sys_redirect_q;
  assign o_act_valid    = act_win;
  assign o_act_taken    = act_win && win_taken;

  always_ff @(posedge clk) begin
    if (rst) begin
      redirect_q    <= 1'b0;
      redirect_pc_q <= {RDA_XLEN{1'b0}};
      sys_redirect_q <= 1'b0;
    end else begin
      // One cycle, always: a redirect the core does not consume must not be
      // re-issued, and a second redirect cannot be raised while the first is
      // still in flight because the ROB is empty until the flush completes.
      redirect_q     <= act_win && win_taken;
      redirect_pc_q  <= (act_win && win_taken)
                        ? (act_sys ? sys_req_pc : req_pc[win_i])
                        : redirect_pc_q;
      sys_redirect_q <= act_sys;
    end
  end

  // ----------------------------------------------------------------- counters
  logic [31:0] req_ctr, act_ctr, drop_ctr, dead_ctr, wait_ctr, nothing_ctr;
  logic [RDA_N-1:0] taken_loser;
  logic [31:0] req_sum, loser_sum, dead_sum;

  always_comb begin
    req_sum   = 32'd0;
    loser_sum = 32'd0;
    dead_sum  = 32'd0;
    for (int unsigned i = 0; i < RDA_N; i++) begin
      taken_loser[i] = act_win && !act_sys && req_taken[win_i] &&
                       (RDA_WIN_W'(i) != win_i) && req_taken[i];
      req_sum   = req_sum   + {31'd0, req_valid[i]};
      loser_sum = loser_sum + {31'd0, taken_loser[i]};
      dead_sum  = dead_sum  + {31'd0, dead[i]};
    end
    // A system request that named an entry the buffer no longer holds is a dead
    // request for the same reason a cluster's is.
    if (sys_dead) dead_sum = dead_sum + 32'd1;
  end

  always_ff @(posedge clk) begin
    if (rst) begin
      req_ctr     <= 32'd0;
      act_ctr     <= 32'd0;
      drop_ctr    <= 32'd0;
      dead_ctr    <= 32'd0;
      wait_ctr    <= 32'd0;
      nothing_ctr <= 32'd0;
    end else begin
      req_ctr     <= req_ctr  + req_sum;
      act_ctr     <= act_ctr  + {31'd0, act_win};
      drop_ctr    <= drop_ctr + loser_sum;
      dead_ctr    <= dead_ctr + dead_sum;
      // A live request that could not act is the evidence that the arbiter waited
      // for older work instead of redirecting early. A live *system* request that
      // could not act is the same statement about a trap or an MRET.
      wait_ctr    <= wait_ctr + {31'd0, ((win_found && !act_win) ||
                                         (sys_cand && !act_sys && !sys_win))};
      nothing_ctr <= nothing_ctr + {31'd0, act_win && !win_taken};
    end
  end

  assign o_req_ctr     = req_ctr;
  assign o_act_ctr     = act_ctr;
  assign o_drop_ctr    = drop_ctr;
  assign o_dead_ctr    = dead_ctr;
  assign o_wait_ctr    = wait_ctr;
  assign o_nothing_ctr = nothing_ctr;

endmodule : mosaic_redirect_arb

`default_nettype wire
