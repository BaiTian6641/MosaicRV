// Simulation wrapper for CASE=reconfigure.drain_and_generation (work package
// I-031).
//
// `mosaic_owner_fsm` is a stateful single-clock module whose control answers
// (`ctrl_ok`, `ctrl_dup`, `ctrl_reject`, `ack_req`, `ack_ok`, `o_stop_admit`,
// `o_publish`) are combinational in this cycle's inputs and the pre-edge state.
// This wrapper adds no timing of its own: every port is passed straight through,
// and there is deliberately no clock generation, no reset generation and no
// `$display` -- the C++ side owns the clock, the reset schedule and all result
// reporting, per sim/common/sim_common.h.
//
// About the widths. This wrapper repeats the module's parameter expressions to
// declare its own port widths, which is a second copy of a derivation, so it is
// worth saying why that is acceptable here and what catches it if it drifts:
//
//   * the widths are *expressions over the generated package*, not literals, so
//     they follow a profile change exactly as the module does;
//   * a wrapper whose port width disagrees with the instance's is an
//     elaboration error in Verilator, not a silently narrower comparison. The
//     failure is loud and immediate.
//
// The geometry is also read back through `o_gen_w`, `o_cnt_w`, `o_seq_w`,
// `o_drain_limit` and `o_states`, and sim/unit/tb_owner_fsm.cpp asserts the
// values it was written for. Those ports exist so the driver's shadow sizes
// itself from the DUT rather than from a second copy of the geometry.

`default_nettype none
`resetall

/* verilator lint_off UNUSEDPARAM */
// The generated header declares one localparam per configuration knob for the
// whole project, and it has an include guard, so this second include of it (the
// module includes it too) is a no-op rather than a duplicate declaration.
`include "mosaic_cfg_pkg.svh"
/* verilator lint_on UNUSEDPARAM */

localparam int unsigned TB_GEN_W = $clog2(mosaic_cfg_pkg::MOSAIC_ROB_ENTRIES) + 1;
localparam int unsigned TB_CNT_W = $clog2(mosaic_cfg_pkg::MOSAIC_ROB_ENTRIES
                                          * mosaic_cfg_pkg::MOSAIC_MAX_UOPS_PER_MACRO + 1);
localparam int unsigned TB_SEQ_W = 8;
localparam int unsigned TB_ST_W  = 3;

module mosaic_owner_fsm_tb (
    input  logic                     clk,
    input  logic                     rst,

    input  logic                     ctrl_valid,
    input  logic [TB_SEQ_W-1:0]      ctrl_seq,
    output logic                     ctrl_ready,
    output logic                     ctrl_ok,
    output logic                     ctrl_dup,
    output logic                     ctrl_reject,

    input  logic                     ack_valid,
    output logic                     ack_req,
    output logic                     ack_ok,
    output logic                     ack_unexpected,

    input  logic                     uop_new,
    input  logic                     uop_done,
    input  logic                     res_new,
    input  logic                     res_done,
    input  logic                     crd_new,
    input  logic                     crd_done,

    output logic [TB_ST_W-1:0]       o_state,
    output logic                     o_stop_admit,
    output logic                     o_busy,
    output logic                     o_publish,
    output logic [TB_GEN_W-1:0]      o_owner_gen,
    output logic [TB_GEN_W-1:0]      o_old_gen,
    output logic [TB_GEN_W-1:0]      o_new_gen,

    output logic [TB_CNT_W-1:0]      o_cnt_uop,
    output logic [TB_CNT_W-1:0]      o_cnt_res,
    output logic [TB_CNT_W-1:0]      o_cnt_crd,
    output logic [TB_CNT_W-1:0]      o_drain_cycles,
    output logic                     o_drain_stall,
    output logic [31:0]              o_ctrl_ok_count,
    output logic [31:0]              o_ctrl_dup_count,
    output logic [31:0]              o_ctrl_reject_count,
    output logic [31:0]              o_ack_ok_count,
    output logic [31:0]              o_ack_unexpected_count,
    output logic [31:0]              o_admit_after_stop_count,
    output logic [31:0]              o_settle_unmatched_count,
    output logic [31:0]              o_publish_count,
    output logic [31:0]              o_abort_count,

    output logic [31:0]              o_gen_w,
    output logic [31:0]              o_cnt_w,
    output logic [31:0]              o_seq_w,
    output logic [31:0]              o_drain_limit,
    output logic [31:0]              o_states
);

  mosaic_owner_fsm u_owner_fsm (
      .clk                      (clk),
      .rst                      (rst),
      .ctrl_valid               (ctrl_valid),
      .ctrl_seq                 (ctrl_seq),
      .ctrl_ready               (ctrl_ready),
      .ctrl_ok                  (ctrl_ok),
      .ctrl_dup                 (ctrl_dup),
      .ctrl_reject              (ctrl_reject),
      .ack_valid                (ack_valid),
      .ack_req                  (ack_req),
      .ack_ok                   (ack_ok),
      .ack_unexpected           (ack_unexpected),
      .uop_new                  (uop_new),
      .uop_done                 (uop_done),
      .res_new                  (res_new),
      .res_done                 (res_done),
      .crd_new                  (crd_new),
      .crd_done                 (crd_done),
      .o_state                  (o_state),
      .o_stop_admit             (o_stop_admit),
      .o_busy                   (o_busy),
      .o_publish                (o_publish),
      .o_owner_gen              (o_owner_gen),
      .o_old_gen                (o_old_gen),
      .o_new_gen                (o_new_gen),
      .o_cnt_uop                (o_cnt_uop),
      .o_cnt_res                (o_cnt_res),
      .o_cnt_crd                (o_cnt_crd),
      .o_drain_cycles           (o_drain_cycles),
      .o_drain_stall            (o_drain_stall),
      .o_ctrl_ok_count          (o_ctrl_ok_count),
      .o_ctrl_dup_count         (o_ctrl_dup_count),
      .o_ctrl_reject_count      (o_ctrl_reject_count),
      .o_ack_ok_count           (o_ack_ok_count),
      .o_ack_unexpected_count   (o_ack_unexpected_count),
      .o_admit_after_stop_count (o_admit_after_stop_count),
      .o_settle_unmatched_count (o_settle_unmatched_count),
      .o_publish_count          (o_publish_count),
      .o_abort_count            (o_abort_count),
      .o_gen_w                  (o_gen_w),
      .o_cnt_w                  (o_cnt_w),
      .o_seq_w                  (o_seq_w),
      .o_drain_limit            (o_drain_limit),
      .o_states                 (o_states)
  );

endmodule : mosaic_owner_fsm_tb

`resetall
`default_nettype wire
