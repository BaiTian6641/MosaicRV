// Simulation wrapper for CASE=fp.operation_matrix (work package I-049).
//
// `mosaic_fpu` is a single-clock unit with registered control state and a
// combinational datapath, so this wrapper adds no timing of its own: the clock
// and the reset schedule belong to the C++ driver (sim/common/sim_common.h), so
// there is no clock generation, no reset generation and no `$display` here.
//
// It does three things of its own, all about geometry and about keeping the
// owner of a fact single:
//
//   * The identity ports are declared with widths derived from the generated
//     identity package, not written out by hand, and the elaborated widths are
//     read back as outputs so the driver sizes its tag model from the DUT.
//     A hand-written "6 bits" would be a second copy of the geometry that a
//     profile change could silently contradict.
//
//   * The operation is passed through as a plain 5-bit vector and cast to the
//     package enumeration once, here, rather than in every port connection.
//
//   * The output names keep the DUT's `o_` prefix where the DUT has one, so the
//     driver and the port list say the same thing twice rather than twice
//     differently.

`default_nettype none
`resetall

/* verilator lint_off UNUSEDPARAM */
/* verilator lint_off UNUSEDSIGNAL */
/* verilator lint_off MODDUP */
// `mosaic_pkg` is included explicitly for the same reason the RTL does: without
// it the package would have to be listed on the runner's command line, which is
// not this case's to change. Both packages carry their own include guards, so
// including them here as well as in the RTL is idempotent.
`include "mosaic_pkg.sv"
`include "mosaic_id_pkg.svh"
/* verilator lint_on MODDUP */
/* verilator lint_on UNUSEDSIGNAL */
/* verilator lint_on UNUSEDPARAM */

localparam int unsigned TB_ROB_INDEX_W = mosaic_id_pkg::MOSAIC_ID_W_ROB_INDEX;
localparam int unsigned TB_ROB_GEN_W   = mosaic_id_pkg::MOSAIC_ID_W_ROB_GEN;
localparam int unsigned TB_UOP_INDEX_W = mosaic_id_pkg::MOSAIC_ID_W_UOP_INDEX;

module mosaic_fpu_tb (
    input  logic                          clk,
    input  logic                          rst,

    // request
    input  logic                          req_valid,
    output logic                          req_ready,
    input  logic [4:0]                    req_op,
    input  logic                          req_fmt,
    input  logic [2:0]                    req_rm,
    input  logic                          req_iw,
    input  logic                          req_is,
    input  logic [63:0]                   req_a,
    input  logic [63:0]                   req_b,
    input  logic [TB_ROB_INDEX_W-1:0]     req_rob_index,
    input  logic [TB_ROB_GEN_W-1:0]       req_rob_gen,
    input  logic [TB_UOP_INDEX_W-1:0]     req_uop_index,

    // cancel
    input  logic                          flush,

    // response
    output logic                          res_valid,
    input  logic                          res_ready,
    output logic [63:0]                   res_data,
    output logic [4:0]                    res_fflags,
    output logic [TB_ROB_INDEX_W-1:0]     res_rob_index,
    output logic [TB_ROB_GEN_W-1:0]       res_rob_gen,
    output logic [TB_UOP_INDEX_W-1:0]     res_uop_index,

    // status and coverage
    output logic                          o_busy,
    output logic [7:0]                    o_latency,
    output logic [6:0]                    o_iter,
    output logic [31:0]                   o_accepted_ctr,
    output logic [31:0]                   o_completed_ctr,
    output logic [31:0]                   o_cancelled_ctr,
    output logic [31:0]                   o_killed_res_ctr,

    // the elaborated identity widths, read back from the generated package
    output logic [31:0]                   o_rob_index_w,
    output logic [31:0]                   o_rob_gen_w,
    output logic [31:0]                   o_uop_index_w
);

  mosaic_pkg::fp_op_e req_op_typed;

  assign req_op_typed = mosaic_pkg::fp_op_e'(req_op);

  mosaic_fpu u_fpu (
      .clk_i             (clk),
      .rst_i             (rst),

      .req_valid_i       (req_valid),
      .req_ready_o       (req_ready),
      .req_op_i          (req_op_typed),
      .req_fmt_i         (req_fmt),
      .req_rm_i          (req_rm),
      .req_iw_i          (req_iw),
      .req_is_i          (req_is),
      .req_a_i           (req_a),
      .req_b_i           (req_b),
      .req_rob_index_i   (req_rob_index),
      .req_rob_gen_i     (req_rob_gen),
      .req_uop_index_i   (req_uop_index),

      .flush_i           (flush),

      .res_valid_o       (res_valid),
      .res_ready_i       (res_ready),
      .res_data_o        (res_data),
      .res_fflags_o      (res_fflags),
      .res_rob_index_o   (res_rob_index),
      .res_rob_gen_o     (res_rob_gen),
      .res_uop_index_o   (res_uop_index),

      .o_busy            (o_busy),
      .o_latency_o       (o_latency),
      .o_iter            (o_iter),
      .o_accepted_ctr    (o_accepted_ctr),
      .o_completed_ctr   (o_completed_ctr),
      .o_cancelled_ctr   (o_cancelled_ctr),
      .o_killed_res_ctr  (o_killed_res_ctr)
  );

  assign o_rob_index_w = 32'(TB_ROB_INDEX_W);
  assign o_rob_gen_w   = 32'(TB_ROB_GEN_W);
  assign o_uop_index_w = 32'(TB_UOP_INDEX_W);

endmodule : mosaic_fpu_tb

`resetall
`default_nettype wire
