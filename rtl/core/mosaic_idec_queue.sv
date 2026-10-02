// ============================================================================
// mosaic_idec_queue -- the decoded-instruction queue (front-end depth).
//
// This replaces the two-entry decode buffer the front end used to hold between
// `mosaic_fetch` and `mosaic_dispatch`. The two entries were justified when
// dispatch allocated exactly one macro per cycle and the buffer only had to
// cover the one-cycle gap between a delivery and its allocation. It is no longer
// enough for two reasons:
//
//   * allocation is two-wide, so dispatch may want *two* decoded instructions in
//     a cycle. A two-entry buffer that is being drained one-at-a-time cannot
//     supply a pair whenever the fetch side happens to be one instruction
//     behind -- which it is, on average, when fetch delivers one per cycle.
//
//   * a redirect used to force the buffer's two entries to be discarded and the
//     whole front end to refill from scratch. A deeper queue lets fetch run
//     ahead of a dispatch stall (a full issue queue, a full ROB, an operand that
//     has not been written), so the bubble a stall used to leave behind is
//     absorbed by the queue instead of stalling the fetch stream.
//
// What it must preserve -- and does -- is the property the compressed work
// depends on (I-041): an instruction's own length and its own bits travel with
// it. Each entry carries `len` and `bits` exactly as `mosaic_fetch` produced
// them; nothing here re-derives either from the decoded control word.
//
// ------------------------------------------------------------------ protocol
//
//   push   one delivery per cycle: `push_valid` with the payload held stable
//          until `push_ready`. `push_ready` is "after this cycle's pop there
//          will be room", so a push can land in a cycle that pops.
//   pop    up to two per cycle, oldest first: `pop_count` is 0, 1 or 2. Slot 0
//          is the oldest entry; slot 1 is the entry immediately behind it. A
//          `pop_count` of 1 with slot 1 valid leaves slot 1 as next cycle's
//          slot 0 -- the queue is a shift register over positions, not a
//          reorder buffer.
//   purge  drop everything (a redirect). It has priority over a push in the same
//          cycle: the delivery being discarded is younger than the redirect and
//          is not architectural.
//   hold   freeze: neither push nor pop. This is the stop/wait state, where a
//          refused macro must stay exactly where it is so the machine stops at
//          the same instruction on the next cycle.
//
// An entry is never written by a push outside `[0, DEPTH)`, and a push is
// accepted only when the post-pop occupancy is below DEPTH, so the occupancy
// cannot exceed DEPTH. The queue holds no architectural state and is not
// flushed per-program; `purge` is the only discarding event besides reset.
// ============================================================================

`default_nettype none
`resetall

/* verilator lint_off UNUSEDPARAM */
`include "mosaic_cfg_pkg.svh"
/* verilator lint_on UNUSEDPARAM */

`include "mosaic_pkg.sv"

module mosaic_idec_queue #(
    parameter int unsigned DEPTH = 8,
    parameter int unsigned XLEN  = mosaic_cfg_pkg::MOSAIC_XLEN,

    localparam int unsigned CNT_W = $clog2(DEPTH + 1)
) (
    input  logic                    clk,
    input  logic                    rst,

    // -------------------------------------------------------------- push side
    input  logic                    push_valid,
    input  logic [XLEN-1:0]         push_pc,
    input  mosaic_pkg::decode_ctl_t push_ctl,
    input  logic [2:0]              push_len,
    input  logic [31:0]             push_bits,
    output logic                    push_ready,

    // --------------------------------------------------------------- pop side
    // `pop_count` is 0, 1 or 2. It is combinational from the queue's own valid
    // bits on the dispatch side, so the pop and the decision that produced it
    // are the same cycle.
    input  logic [1:0]              pop_count,

    output logic                    out0_valid,
    output logic [XLEN-1:0]         out0_pc,
    output mosaic_pkg::decode_ctl_t out0_ctl,
    output logic [2:0]              out0_len,
    output logic [31:0]             out0_bits,

    output logic                    out1_valid,
    output logic [XLEN-1:0]         out1_pc,
    output mosaic_pkg::decode_ctl_t out1_ctl,
    output logic [2:0]              out1_len,
    output logic [31:0]             out1_bits,

    // ------------------------------------------------------------- control
    input  logic                    purge,
    input  logic                    hold,

    // --------------------------------------------------------- observation
    output logic [CNT_W-1:0]        count,
    output logic                    full
);

  typedef struct packed {
    logic [XLEN-1:0]         pc;
    mosaic_pkg::decode_ctl_t ctl;
    logic [2:0]              len;
    logic [31:0]             bits;
  } idec_ent_t;

  idec_ent_t            ent   [0:DEPTH-1];
  logic [DEPTH-1:0]     valid;
  logic [CNT_W-1:0]     occ;

  // The pop is clamped to the occupancy. Dispatch never asks for more than it
  // can see, and the clamp makes that a structural property here rather than a
  // contract the caller has to honour for the arithmetic below to stay in range.
  integer               pop_i;
  logic                 room_after_pop;

  idec_ent_t            ent_n [0:DEPTH-1];
  logic [DEPTH-1:0]     valid_n;
  logic [CNT_W-1:0]     occ_n;

  always_comb begin
    // Integer arithmetic with an explicit clamp: the pop can never exceed the
    // occupancy, and saying so here keeps every index below in range without a
    // width cast that could truncate.
    pop_i = int'(pop_count);
    if (pop_i > int'(occ)) pop_i = int'(occ);
    room_after_pop = ((int'(occ) - pop_i) < int'(DEPTH));
  end

  assign push_ready = room_after_pop;
  assign count      = occ;
  assign full       = (occ == CNT_W'(DEPTH));

  // ------------------------------------------------------------- next state
  // One expression per position: shift down by the pop, then place the push at
  // the tail the pop left. Positions at or beyond the occupancy are invalidated,
  // so a stale entry can never be offered as slot 1.
  integer si;
  integer fi;
  always_comb begin
    for (si = 0; si < DEPTH; si = si + 1) begin
      if ((si + pop_i) < int'(occ)) begin
        ent_n[si]   = ent[si+pop_i];
        valid_n[si] = valid[si+pop_i];
      end else begin
        ent_n[si]   = ent[si];
        valid_n[si] = 1'b0;
      end
    end
    occ_n = CNT_W'(int'(occ) - pop_i);
    if (push_valid && push_ready) begin
      ent_n[int'(occ) - pop_i].pc   = push_pc;
      ent_n[int'(occ) - pop_i].ctl  = push_ctl;
      ent_n[int'(occ) - pop_i].len  = push_len;
      ent_n[int'(occ) - pop_i].bits = push_bits;
      valid_n[int'(occ) - pop_i]    = 1'b1;
      occ_n                         = occ_n + CNT_W'(1);
    end
  end

  always_ff @(posedge clk) begin
    if (rst) begin
      occ   <= {CNT_W{1'b0}};
      valid <= {DEPTH{1'b0}};
    end else if (purge) begin
      occ   <= {CNT_W{1'b0}};
      valid <= {DEPTH{1'b0}};
    end else if (!hold) begin
      occ   <= occ_n;
      valid <= valid_n;
      for (fi = 0; fi < DEPTH; fi = fi + 1) begin
        ent[fi] <= ent_n[fi];
      end
    end
  end

  // --------------------------------------------------------------- outputs
  assign out0_valid = valid[0];
  assign out0_pc    = ent[0].pc;
  assign out0_ctl   = ent[0].ctl;
  assign out0_len   = ent[0].len;
  assign out0_bits  = ent[0].bits;

  assign out1_valid = valid[1];
  assign out1_pc    = ent[1].pc;
  assign out1_ctl   = ent[1].ctl;
  assign out1_len   = ent[1].len;
  assign out1_bits  = ent[1].bits;

endmodule : mosaic_idec_queue

`default_nettype wire
