// ============================================================================
// mosaic_lane_broker -- runtime lane-quota reallocation (work package I-059).
//
// The vector engine has a *lane quota*: 2, 4 or 8 lanes' worth of the VRF read
// share. The quota is a throughput knob and nothing else -- it never changes
// which elements exist, only how many lanes work on them -- so a change to it
// is safe exactly when it lands at a **vector instruction boundary**, after the
// macro in flight has drained. A change that landed inside a macro would be
// the fail mode the card names: "shut a lane down" read as "discard the
// elements that lane still owed".
//
// This module is that broker, and it is deliberately the *same protocol* work
// package I-031 froze and proved: it **instantiates `mosaic_owner_fsm`** and
// binds its three settle classes to the vector engine's three obligations:
//
//   * `uop` -- a vector macro is staged and not yet retired (one at a time,
//     because the allocation barrier serializes vector macros);
//   * `res` -- a launched macro's element work is still running in the ALU or
//     the packetizer;
//   * `crd` -- a macro has finished its elements but its completion has not
//     yet been collected by the writeback path.
//
// The FSM's `STOP_ADMIT -> DRAIN -> ACK -> PUBLISH` is then exactly the
// boundary rule: `o_stop_admit` gates new macro admission, DRAIN waits for
// every class to settle, ACK waits for the engine's own statement that its lane
// state for the *old* quota is quiesced, and PUBLISH commits the new quota and
// advances the generation. The generation is the FSM's, and every published
// quota differs from its predecessor by that generation -- an id from the old
// quota cannot alias one from the new.
//
// --------------------------------------------------------------- the request
//
// The request is a non-architectural control port (`req_valid_i` +
// `req_quota_i`), not a CSR: the quota is a resource share, not architectural
// state, and `vl`/`vlenb` must not move when it changes. The broker triggers an
// owner-FSM message when the presented value differs from the last one it
// accepted and is one of the three legal shares. A held request is idempotent:
// after it is accepted the trigger is quiet until the value changes again.
//
// ------------------------------------------------------------- the evidence
//
// The case needs to see the *boundary*, not just the outcome, so the broker
// reports: the committed quota, the request it is holding, the generation, the
// one-cycle publish, whether a macro was live when a request was accepted
// (`o_req_mid_macro_ctr`), and the acknowledgements it demanded and received.
// `o_pub_mid_macro_ctr` is the direct statement of the boundary rule: it counts
// publishes that happened while a macro was live, and the shipping build can
// only ever report zero.
//
// ------------------------------------------------------------------ mutants
//
// `-DMOSAIC_LANE_MUTANT_MID_MACRO` commits the requested quota the cycle it is
// presented, without draining -- the "resize applied mid-macro" defect. The
// shipping build defines none of the broker's mutants; the table with real
// output is in results/reports/I-059-lane-broker.md. The acknowledgement
// mutant is I-031's own `MOSAIC_OWNER_MUTANT_NO_ACK`, reused here rather than
// re-invented: it is what "publish without the participant's acknowledgement"
// already means.
// ============================================================================

`default_nettype none
`resetall

/* verilator lint_off UNUSEDPARAM */
// The generated header declares one localparam per configuration knob for the
// whole project; this module names two of them.
`include "mosaic_cfg_pkg.svh"
/* verilator lint_on UNUSEDPARAM */

module mosaic_lane_broker #(
    // The quota in force out of reset. 8 is the widest legal share, so the
    // machine starts at full width and only ever narrows on software's request.
    parameter logic [3:0] QUOTA_RESET = 4'd8
) (
    input  logic             clk,
    input  logic             rst,

    // ------------------------------------------------------- runtime request
    input  logic             req_valid_i,
    input  logic [3:0]       req_quota_i,   // 2, 4 or 8; anything else is ignored

    // ------------------------------------------- the engine's obligations
    // One event pair per settle class, exactly the FSM's three.
    input  logic             macro_live_i,  // a macro is staged or running (evidence)
    input  logic             macro_new_i,
    input  logic             macro_done_i,
    input  logic             elem_new_i,
    input  logic             elem_done_i,
    input  logic             wb_new_i,
    input  logic             wb_done_i,

    // The engine's own statement that its lane state for the old quota is
    // quiesced. The FSM will not publish without it.
    input  logic             ack_i,

    // ------------------------------------------------------------- outputs
    output logic [3:0]       o_quota,          // the committed share
    output logic [3:0]       o_req_quota,      // the request being held
    output logic             o_stop_admit,
    output logic             o_busy,
    output logic             o_publish,        // one-cycle pulse in PUBLISH
    output logic [7:0]       o_gen,
    output logic             o_ack_req,
    output logic             o_ack_seen,
    output logic [31:0]      o_publish_ctr,
    output logic [31:0]      o_ack_req_ctr,
    output logic [31:0]      o_ack_ctr,
    output logic [31:0]      o_req_mid_macro_ctr,
    output logic [31:0]      o_pub_mid_macro_ctr,
    output logic [31:0]      o_abort_ctr,
    output logic [31:0]      o_drain_stall,
    output logic [3:0]       o_state,
    output logic [3:0]       o_quota_reset,
    output logic [3:0]       o_quota_max,
    output logic [7:0]       o_gen_w
);

  // The FSM's generation and count widths are its own `localparam`s, derived
  // from the profile; naming them here from the same package is the only way to
  // declare the intermediate wires without inventing a second width rule.
  localparam int unsigned FSM_GEN_W = $clog2(mosaic_cfg_pkg::MOSAIC_ROB_ENTRIES) + 1;
  localparam int unsigned FSM_CNT_W = $clog2(mosaic_cfg_pkg::MOSAIC_ROB_ENTRIES
                                             * mosaic_cfg_pkg::MOSAIC_MAX_UOPS_PER_MACRO + 1);

  // The widest share this profile supports. The quota is a lane count, so it is
  // bounded by the vector geometry's lane maximum; the profile declares eight.
  localparam logic [3:0] QUOTA_MAX = 4'd8;
  localparam logic [3:0] QUOTA_MIN = 4'd2;

  // ------------------------------------------------------------ FSM wires
  logic                    fsm_ctrl_valid;
  logic [7:0]              fsm_ctrl_seq;
  logic                    fsm_ctrl_ready, fsm_ctrl_ok, fsm_ctrl_dup, fsm_ctrl_reject;
  logic                    fsm_ack_valid, fsm_ack_req, fsm_ack_ok, fsm_ack_unexpected;
  logic                    fsm_stop_admit, fsm_busy, fsm_publish;
  logic [FSM_GEN_W-1:0]    fsm_gen, fsm_old_gen, fsm_new_gen;
  logic [FSM_CNT_W-1:0]    fsm_cnt_uop, fsm_cnt_res, fsm_cnt_crd, fsm_drain_cycles;
  logic                    fsm_drain_stall;
  logic [31:0]             fsm_publish_ctr, fsm_ack_ok_ctr, fsm_abort_ctr;
  logic [31:0]             fsm_states;

  // ------------------------------------------------------- request tracking
  logic [3:0] quota_q;      // committed
  logic [3:0] last_req_q;   // the last request value accepted
  logic [3:0] held_q;       // the value waiting to be published
  logic [7:0] seq_q;

  // A request is presented when the value differs from the last accepted one and
  // is one of the legal shares. The FSM's own ready says whether it may be
  // accepted *now*; while it is busy the request simply waits.
  logic req_pending;
  always_comb begin
    req_pending = req_valid_i && (req_quota_i != last_req_q) &&
                  (req_quota_i >= QUOTA_MIN) && (req_quota_i <= QUOTA_MAX) &&
                  ((req_quota_i == 4'd2) || (req_quota_i == 4'd4) ||
                   (req_quota_i == 4'd8));
  end

  assign fsm_ctrl_valid = req_pending && fsm_ctrl_ready;
  assign fsm_ctrl_seq   = seq_q;
  assign fsm_ack_valid  = ack_i;

  // The obligation events. A macro that leaves settles every class it still
  // owes -- a redirected or faulted macro never runs its remaining elements,
  // and the drain must not hang on them. `fsm_cnt_res`/`fsm_cnt_crd` are
  // registered, so this is a registered, not a combinational, loop.
  logic res_done_eff;
  logic crd_done_eff;
  assign res_done_eff = elem_done_i || (macro_done_i && (fsm_cnt_res != {FSM_CNT_W{1'b0}}));
  assign crd_done_eff = wb_done_i || (macro_done_i && (fsm_cnt_crd != {FSM_CNT_W{1'b0}}));

  mosaic_owner_fsm u_owner (
      .clk              (clk),
      .rst              (rst),
      .ctrl_valid       (fsm_ctrl_valid),
      .ctrl_seq         (fsm_ctrl_seq),
      .ctrl_ready       (fsm_ctrl_ready),
      .ctrl_ok          (fsm_ctrl_ok),
      .ctrl_dup         (fsm_ctrl_dup),
      .ctrl_reject      (fsm_ctrl_reject),
      .ack_valid        (fsm_ack_valid),
      .ack_req          (fsm_ack_req),
      .ack_ok           (fsm_ack_ok),
      .ack_unexpected   (fsm_ack_unexpected),
      .uop_new          (macro_new_i),
      .uop_done         (macro_done_i),
      .res_new          (elem_new_i),
      .res_done         (res_done_eff),
      .crd_new          (wb_new_i),
      .crd_done         (crd_done_eff),
      .o_state          (),
      .o_stop_admit     (fsm_stop_admit),
      .o_busy           (fsm_busy),
      .o_publish        (fsm_publish),
      .o_owner_gen      (fsm_gen),
      .o_old_gen        (fsm_old_gen),
      .o_new_gen        (fsm_new_gen),
      .o_cnt_uop        (fsm_cnt_uop),
      .o_cnt_res        (fsm_cnt_res),
      .o_cnt_crd        (fsm_cnt_crd),
      .o_drain_cycles   (fsm_drain_cycles),
      .o_drain_stall    (fsm_drain_stall),
      .o_ctrl_ok_count  (),
      .o_ctrl_dup_count (),
      .o_ctrl_reject_count (),
      .o_ack_ok_count   (fsm_ack_ok_ctr),
      .o_ack_unexpected_count (),
      .o_admit_after_stop_count (),
      .o_settle_unmatched_count (),
      .o_publish_count  (fsm_publish_ctr),
      .o_abort_count    (fsm_abort_ctr),
      .o_gen_w          (),
      .o_cnt_w          (),
      .o_seq_w          (),
      .o_drain_limit    (),
      .o_states         (fsm_states)
  );

  // --------------------------------------------------------------- evidence
  logic        ack_req_q;
  logic [31:0] ack_req_ctr;
  logic [31:0] req_mid_macro_ctr;
  logic [31:0] pub_mid_macro_ctr;

`ifdef MOSAIC_LANE_MUTANT_MID_MACRO
  // NEGATIVE CONTROL: the requested share takes effect the cycle it is
  // presented, without the drain. A resize requested inside a macro then moves
  // the visible quota while that macro is still running -- the "resize applied
  // mid-macro" fail mode -- and CASE=rvv.lane_resize_boundary sees a quota
  // change with a macro live.
  assign o_quota         = req_valid_i ? req_quota_i : quota_q;
`else
  assign o_quota         = quota_q;
`endif
  assign o_req_quota     = held_q;
  assign o_stop_admit    = fsm_stop_admit;
  assign o_busy          = fsm_busy;
  assign o_publish       = fsm_publish;
  assign o_gen           = 8'(fsm_gen);
  assign o_ack_req       = fsm_ack_req;
  assign o_ack_seen      = ack_i;
  assign o_publish_ctr   = fsm_publish_ctr;
  assign o_ack_req_ctr   = ack_req_ctr;
  assign o_ack_ctr       = fsm_ack_ok_ctr;
  assign o_req_mid_macro_ctr = req_mid_macro_ctr;
  assign o_pub_mid_macro_ctr = pub_mid_macro_ctr;
  assign o_abort_ctr     = fsm_abort_ctr;
  assign o_drain_stall   = {31'd0, fsm_drain_stall};
  assign o_state         = 4'(fsm_states);
  assign o_quota_reset   = QUOTA_RESET;
  assign o_quota_max     = QUOTA_MAX;
  assign o_gen_w         = 8'(FSM_GEN_W);

  always_ff @(posedge clk) begin
    if (rst) begin
      quota_q            <= QUOTA_RESET;
      last_req_q         <= QUOTA_RESET;
      held_q             <= QUOTA_RESET;
      seq_q              <= 8'd0;
      ack_req_q          <= 1'b0;
      ack_req_ctr        <= 32'd0;
      req_mid_macro_ctr  <= 32'd0;
      pub_mid_macro_ctr  <= 32'd0;
    end else begin
      if (fsm_ctrl_ok) begin
        held_q            <= req_quota_i;
        last_req_q        <= req_quota_i;
        seq_q             <= seq_q + 8'd1;
        if (macro_live_i) req_mid_macro_ctr <= req_mid_macro_ctr + 32'd1;
      end

      if (fsm_publish) begin
        quota_q <= held_q;
        if (macro_live_i) pub_mid_macro_ctr <= pub_mid_macro_ctr + 32'd1;
      end

      // Count the *entries* into ACK, so the case can state that every resize
      // demanded an acknowledgement rather than that one happened to arrive.
      ack_req_q <= fsm_ack_req;
      if (fsm_ack_req && !ack_req_q) ack_req_ctr <= ack_req_ctr + 32'd1;
    end
  end

endmodule : mosaic_lane_broker

`resetall
`default_nettype wire
