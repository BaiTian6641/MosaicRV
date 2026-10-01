// Simulation wrapper for CASE=fetch.redirect_late_response (work package I-009).
//
// `mosaic_fetch` owns one mosaic_predictor instance and forwards its update and
// RAS-recovery ports, so this wrapper is almost pure pass-through: every port is
// broken out one signal wide so the C++ driver can name the field that
// mismatched rather than comparing a packed vector.
//
// The one thing the wrapper adds is geometry. The elaborated widths are read
// back from the *instance parameters* rather than from a second `include of the
// generated package, so the C++ shadow sizes itself from the hardware that was
// actually built. A profile with a different fetch_outstanding or ROB depth
// therefore needs no edit here and no number in the driver that has to be kept
// in step with the RTL.
//
// Note what is *not* here: no request generator, no response queue, no memory
// model. The C++ side is the requester and the responder, which is what makes the
// "late response" case constructible at all -- a self-driven fetch unit could
// never be made to answer a redirect in a chosen order.
//
// There is deliberately no clock generation, no reset generation and no
// `$display` in here: the C++ side owns the clock, the reset schedule and all
// result reporting, per sim/common/sim_common.h.

`default_nettype none
`resetall

// The generated header is included here as well as in the RTL. That is not a
// second copy of the geometry -- it is the *same* geometry, from the same
// generated package, and the wrapper needs the three derived widths to declare
// ports that match mosaic_fetch's exactly. Verilator treats a width mismatch on
// an instance connection as fatal, so a wrapper that padded the narrow fields up
// to a fixed 8 bits would not build at all; the alternative, hardcoding the
// numbers here, would be a second copy that could drift from the profile.
//
// The widths are still read back out of the *elaborated instance* below, so the
// C++ driver still learns the geometry from the hardware rather than from this
// file, and the two sources can be compared against each other.
`include "mosaic_cfg_pkg.svh"

module mosaic_fetch_tb #(
    // The three derived widths, with the same expressions mosaic_fetch uses for
    // its own ports. `localparam` in a parameter port list is not visible to the
    // port declarations that follow on either tool, so these are parameters.
    parameter int unsigned ID_W = (mosaic_cfg_pkg::MOSAIC_FETCH_OUTSTANDING <= 1)
                                  ? 1 : $clog2(mosaic_cfg_pkg::MOSAIC_FETCH_OUTSTANDING),
    parameter int unsigned CNT_W = $clog2(mosaic_cfg_pkg::MOSAIC_FETCH_OUTSTANDING + 1),
    parameter int unsigned EPOCH_W = (mosaic_cfg_pkg::MOSAIC_ROB_ENTRIES <= 1)
                                     ? 2 : $clog2(mosaic_cfg_pkg::MOSAIC_ROB_ENTRIES) + 1
) (
    input  logic        clk,
    input  logic        rst,

    // request issue
    input  logic        req_valid,
    input  logic [63:0] req_pc,
    output logic        req_ready,
    output logic [ID_W-1:0]  req_id,
    output logic [EPOCH_W-1:0]  req_epoch,

    // response intake
    input  logic        rsp_valid,
    output logic        rsp_ready,
    output logic        rsp_squashed,
    input  logic [ID_W-1:0]  rsp_id,
    input  logic [EPOCH_W-1:0]  rsp_epoch,
    input  logic [31:0] rsp_data,
    input  logic [2:0]  rsp_len,
    input  logic        rsp_fault,

    // redirect
    input  logic        redirect_valid,
    input  logic [63:0] redirect_pc,

    // predictor query (forwarded)
    input  logic        pred_valid,
    input  logic [63:0] pred_pc,
    input  logic        pred_is_branch,
    input  logic        pred_is_jump,
    input  logic        pred_is_return,

    // predictor update and RAS recovery (forwarded)
    input  logic        upd_valid,
    input  logic [63:0] upd_pc,
    input  logic        upd_is_branch,
    input  logic        upd_is_jump,
    input  logic        upd_is_call,
    input  logic        upd_is_return,
    input  logic        upd_is_taken,
    input  logic [63:0] upd_target,
    input  logic        ckpt_valid,
    input  logic        flush,

    // predictor reports and the derived next PC
    output logic        pred_next_valid,
    output logic [63:0] pred_next_pc,
    output logic        pred_squashed,
    output logic        pred_taken,
    output logic        pred_btb_hit,
    output logic        pred_btb_miss,
    output logic        pred_ras_valid,
    output logic        pred_ras_underflow,
    // The predictor's RAS edge reports, forwarded by fetch. Nothing in this
    // case acts on them; they are broken out so that a connection failure would
    // be a build error rather than an unconnected pin.
    output logic        ras_overflow,
    output logic        ras_underflow,

    // delivered instruction event
    output logic        out_valid,
    input  logic        out_ready,
    output logic [63:0] out_pc,
    output logic [31:0] out_bits,
    output logic [2:0]  out_len,
    output logic        out_illegal,
    output logic        out_fault,
    output logic [63:0] out_cause,

    // observability
    output logic [CNT_W-1:0]  outstanding_count,
    output logic [CNT_W-1:0]  cancel_pending,
    output logic [EPOCH_W-1:0]  epoch_now,
    output logic [31:0] issued_count,
    output logic [31:0] accept_count,
    output logic [31:0] drop_count,
    output logic [31:0] stale_drop_count,
    output logic [31:0] squashed_drop_count,
    output logic [31:0] credit_drop_count,
    output logic [31:0] delivered_count,
    output logic [31:0] fault_count,
    output logic [31:0] illegal_count,
    output logic [31:0] deny_count,
    output logic [31:0] cancel_count,
    output logic [63:0] fetch_pc,
    // mosaic_fetch's own observation bundle (the response classification and the
    // output register). Brought out so the pin is not left dangling; the
    // redirect/recovery cases read it through mosaic_core.
    output logic [127:0] o_dbg_state,

    // The elaborated geometry, read back from the DUT instance.
    output logic [31:0] o_fetch_outstanding,
    output logic [31:0] o_epoch_w,
    output logic [31:0] o_id_w,
    output logic [31:0] o_cnt_w,
    output logic [31:0] o_xlen
);

  mosaic_fetch #(
      .ID_W    (ID_W),
      .CNT_W   (CNT_W),
      .EPOCH_W (EPOCH_W)
  ) u_fetch (
      .clk                 (clk),
      .rst                 (rst),

      .req_valid           (req_valid),
      .req_pc              (req_pc),
      .req_ready           (req_ready),
      .req_id              (req_id),
      .req_epoch           (req_epoch),

      .rsp_valid           (rsp_valid),
      .rsp_ready           (rsp_ready),
      .rsp_squashed        (rsp_squashed),
      .rsp_id              (rsp_id),
      .rsp_epoch           (rsp_epoch),
      .rsp_data            (rsp_data),
      .rsp_len             (rsp_len),
      .rsp_fault           (rsp_fault),

      .redirect_valid      (redirect_valid),
      .redirect_pc         (redirect_pc),

      .pred_valid          (pred_valid),
      .pred_pc             (pred_pc),
      .pred_is_branch      (pred_is_branch),
      .pred_is_jump        (pred_is_jump),
      .pred_is_return      (pred_is_return),

      .upd_valid           (upd_valid),
      .upd_pc              (upd_pc),
      .upd_is_branch       (upd_is_branch),
      .upd_is_jump         (upd_is_jump),
      .upd_is_call         (upd_is_call),
      .upd_is_return       (upd_is_return),
      .upd_is_taken        (upd_is_taken),
      .upd_target          (upd_target),
      .ckpt_valid          (ckpt_valid),
      .flush               (flush),

      .pred_next_valid     (pred_next_valid),
      .pred_next_pc        (pred_next_pc),
      .pred_squashed       (pred_squashed),
      .pred_taken          (pred_taken),
      .pred_btb_hit        (pred_btb_hit),
      .pred_btb_miss       (pred_btb_miss),
      .pred_ras_valid      (pred_ras_valid),
      .pred_ras_underflow  (pred_ras_underflow),
      .ras_overflow        (ras_overflow),
      .ras_underflow       (ras_underflow),

      .out_valid           (out_valid),
      .out_ready           (out_ready),
      .out_pc              (out_pc),
      .out_bits            (out_bits),
      .out_len             (out_len),
      .out_illegal         (out_illegal),
      .out_fault           (out_fault),
      .out_cause           (out_cause),

      .outstanding_count   (outstanding_count),
      .cancel_pending      (cancel_pending),
      .epoch_now           (epoch_now),
      .issued_count        (issued_count),
      .accept_count        (accept_count),
      .drop_count          (drop_count),
      .stale_drop_count    (stale_drop_count),
      .squashed_drop_count (squashed_drop_count),
      .credit_drop_count   (credit_drop_count),
      .delivered_count     (delivered_count),
      .fault_count         (fault_count),
      .illegal_count       (illegal_count),
      .deny_count          (deny_count),
      .cancel_count        (cancel_count),
      .fetch_pc            (fetch_pc),
      .o_dbg_state         (o_dbg_state)
  );

  // Read back from the instance rather than from a second `include of the
  // generated package. One include of the generated header, in the RTL, and the
  // driver learns the sizes from the hardware that exists.
  assign o_fetch_outstanding = 32'(u_fetch.FETCH_OUTSTANDING);
  assign o_epoch_w            = 32'(u_fetch.EPOCH_W);
  assign o_id_w               = 32'(u_fetch.ID_W);
  assign o_cnt_w              = 32'(u_fetch.CNT_W);
  assign o_xlen               = 32'(u_fetch.XLEN);

endmodule : mosaic_fetch_tb

`resetall
`default_nettype wire
