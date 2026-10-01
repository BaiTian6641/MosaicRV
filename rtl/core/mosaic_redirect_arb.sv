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

    // ------------------------------------------------- the ROB's head view
    input  logic                       head_valid,
    input  logic [RDA_IDX_W-1:0]       head_index,
    input  logic [RDA_RGEN_W-1:0]      head_gen,
    input  logic [RDA_OCC_W-1:0]       head_occupied,
    input  logic                       head_retire, // the head leaves this cycle

    // ------------------------------------------------------------- the action
    // A one-cycle pulse in the cycle *after* the redirecting head retired, so
    // the flush it triggers cannot also erase the instruction that caused it.
    output logic                       redirect_valid,
    output logic [RDA_XLEN-1:0]        redirect_pc,

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
      // A live entry at the head must carry the head's generation; a different
      // generation at the same index means the slot has been recycled and the
      // request belongs to an instruction that no longer exists.
      dead[i] = req_valid[i] && !cand[i];
      if (req_valid[i] && cand[i] && (off[i] == {RDA_IDX_W{1'b0}}) &&
          (req_rob_gen[i] != head_gen)) begin
        dead[i] = 1'b1;
        cand[i] = 1'b0;
      end
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
  logic act;

`ifdef MOSAIC_REDIRECT_MUTANT_NO_HEAD_WAIT
  // NEGATIVE CONTROL 2: the winner acts immediately, without waiting for its
  // macro to reach the head. Everything older than it in the ROB is still in
  // flight and is about to be squashed by a restore that does not cover it.
  assign act = win_found;
`else
  assign act = win_found && head_valid && head_retire &&
               (off[win_i] == {RDA_IDX_W{1'b0}}) &&
               (req_rob_gen[win_i] == head_gen);
`endif

  always_comb begin
    for (int unsigned i = 0; i < RDA_N; i++) begin
      req_ack[i] = 1'b0;
      if (dead[i]) begin
        req_ack[i] = 1'b1;
      end
      if (act && (RDA_WIN_W'(i) == win_i)) begin
        req_ack[i] = 1'b1;
      end
      // Young taken losers are wrong-path: the winner's redirect flushes them.
      // A not-taken winner drops nothing -- a younger taken branch behind a
      // not-taken one is on the correct path and must keep its request.
      if (act && req_taken[win_i] && (RDA_WIN_W'(i) != win_i) && req_taken[i]) begin
        req_ack[i] = 1'b1;
      end
    end
  end

  // ------------------------------------------------------- registered pulse
  logic              redirect_q;
  logic [RDA_XLEN-1:0] redirect_pc_q;

  assign redirect_valid = redirect_q;
  assign redirect_pc    = redirect_pc_q;

  always_ff @(posedge clk) begin
    if (rst) begin
      redirect_q    <= 1'b0;
      redirect_pc_q <= {RDA_XLEN{1'b0}};
    end else begin
      // One cycle, always: a redirect the core does not consume must not be
      // re-issued, and a second redirect cannot be raised while the first is
      // still in flight because the ROB is empty until the flush completes.
      redirect_q    <= act && req_taken[win_i];
      redirect_pc_q <= (act && req_taken[win_i]) ? req_pc[win_i] : redirect_pc_q;
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
      taken_loser[i] = act && req_taken[win_i] && (RDA_WIN_W'(i) != win_i) && req_taken[i];
      req_sum   = req_sum   + {31'd0, req_valid[i]};
      loser_sum = loser_sum + {31'd0, taken_loser[i]};
      dead_sum  = dead_sum  + {31'd0, dead[i]};
    end
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
      act_ctr     <= act_ctr  + {31'd0, act};
      drop_ctr    <= drop_ctr + loser_sum;
      dead_ctr    <= dead_ctr + dead_sum;
      // A live request that could not act is the evidence that the arbiter waited
      // for older work instead of redirecting early.
      wait_ctr    <= wait_ctr + {31'd0, (win_found && !act)};
      nothing_ctr <= nothing_ctr + {31'd0, act && !req_taken[win_i]};
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
