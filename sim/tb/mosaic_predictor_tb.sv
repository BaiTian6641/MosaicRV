// Simulation wrapper for CASE=predictor.btb_aliasing (work package I-021).
//
// `mosaic_predictor` is combinational on its query port and registered on its
// update port, so this wrapper adds no timing of its own: it passes the query
// and update ports straight through and breaks the outputs out one signal per
// port so the C++ driver can name the field that mismatched rather than
// comparing a packed vector.
//
// The one thing this wrapper adds is the geometry. The three depth outputs are
// read back from the *elaborated* instance parameters, so the C++ shadow model
// sizes itself from what the hardware was actually built with. If the geometry
// file changes, the shadow changes with it on the next run; there is no number
// in the driver that has to be remembered in step with the RTL. The only way
// for the two to disagree is for the driver to ignore these ports, which the
// first check after reset makes impossible.
//
// There is deliberately no clock generation, no reset generation and no
// `$display` in here: the C++ side owns the clock, the reset schedule and all
// result reporting, per sim/common/sim_common.h.

`default_nettype none
`resetall

module mosaic_predictor_tb (
    input  logic        clk,
    input  logic        rst,

    // query port
    input  logic [63:0] q_pc,
    input  logic        q_valid,
    input  logic        q_is_branch,
    input  logic        q_is_jump,
    input  logic        q_is_return,

    // update port
    input  logic        upd_valid,
    input  logic [63:0] upd_pc,
    input  logic        upd_is_branch,
    input  logic        upd_is_jump,
    input  logic        upd_is_call,
    input  logic        upd_is_return,
    input  logic        upd_taken,
    input  logic [63:0] upd_target,

    // RAS recovery control
    input  logic        ckpt_valid,
    input  logic        flush,

    // query outputs
    output logic        pred_taken,
    output logic [63:0] pred_target,
    output logic        pred_redirect,
    output logic        pred_btb_hit,
    output logic        pred_btb_miss,
    output logic        pred_ras_valid,
    output logic        pred_ras_underflow,

    // update outputs
    output logic        ras_overflow,
    output logic        ras_underflow,

    // The elaborated geometry, read back from the DUT instance.
    output logic [31:0] o_bpu_entries,
    output logic [31:0] o_btb_entries,
    output logic [31:0] o_ras_entries,
    output logic [31:0] o_xlen
);

  mosaic_predictor u_pred (
      .clk                 (clk),
      .rst                 (rst),

      .q_pc                (q_pc),
      .q_valid             (q_valid),
      .q_is_branch         (q_is_branch),
      .q_is_jump           (q_is_jump),
      .q_is_return         (q_is_return),

      .upd_valid           (upd_valid),
      .upd_pc              (upd_pc),
      .upd_is_branch       (upd_is_branch),
      .upd_is_jump         (upd_is_jump),
      .upd_is_call         (upd_is_call),
      .upd_is_return       (upd_is_return),
      .upd_taken           (upd_taken),
      .upd_target          (upd_target),

      .ckpt_valid          (ckpt_valid),
      .flush               (flush),

      .pred_taken          (pred_taken),
      .pred_target         (pred_target),
      .pred_redirect       (pred_redirect),
      .pred_btb_hit        (pred_btb_hit),
      .pred_btb_miss       (pred_btb_miss),
      .pred_ras_valid      (pred_ras_valid),
      .pred_ras_underflow  (pred_ras_underflow),

      .ras_overflow        (ras_overflow),
      .ras_underflow       (ras_underflow)
  );

  // Read back from the instance rather than from a second `include of the
  // generated package. One include of the generated header, in the RTL, and the
  // driver learns the sizes from the hardware that exists.
  assign o_bpu_entries = 32'(u_pred.BPU_ENTRIES);
  assign o_btb_entries = 32'(u_pred.BTB_ENTRIES);
  assign o_ras_entries = 32'(u_pred.RAS_ENTRIES);
  assign o_xlen        = 32'(u_pred.XLEN);

endmodule : mosaic_predictor_tb

`resetall
`default_nettype wire
