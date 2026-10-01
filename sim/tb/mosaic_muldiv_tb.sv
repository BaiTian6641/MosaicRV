// Simulation wrapper for CASE=muldiv.kill_and_edges (work package I-012).
//
// `mosaic_muldiv` is a stateful single-clock unit: every port is registered or
// combinational in the registers, and this wrapper adds no timing of its own.
// The clock and the reset schedule belong to the C++ driver, per
// sim/common/sim_common.h, so there is no clock generation, no reset generation
// and no `$display` in here.
//
// It does two things of its own, both about geometry:
//
//   * The identity ports are declared with widths **derived from the generated
//     identity package**, not written out by hand. A hand-written "6 bits" in a
//     testbench is a second copy of the geometry that a profile change would
//     silently contradict, and a mismatch there shows up as a truncation the
//     driver never looks at -- a test that got weaker without failing.
//
//   * The elaborated widths are read back out as outputs, so the C++ driver
//     sizes its shadow model from the DUT and from nothing else. The driver
//     cannot disagree with the hardware about how wide an identity is.
//
// The output names keep the DUT's `o_` prefix so that the driver and the port
// list say the same thing twice rather than twice differently.

`default_nettype none
`resetall

/* verilator lint_off UNUSEDPARAM */
/* verilator lint_off UNUSEDSIGNAL */
/* verilator lint_off MODDUP */
// The generated identity header declares one width per identity field for the
// whole project, and it is included here as well as in the RTL because this
// wrapper's port list needs the same widths. It carries no include guard, so the
// second inclusion in one compilation unit is a duplicate package declaration
// and nothing else -- the two copies are the same generated text, so there is
// nothing for them to disagree about. Verilator deduplicates packages and warns
// (MODDUP); the alternative, a locally-defined guard macro, would make the
// second includer silently miss the package if the first were ever removed,
// which is a worse failure than the warning.
//
// UNUSEDSIGNAL is scoped to the include for the same reason as in
// rtl/core/mosaic_muldiv.sv: the package's own helpers leave three bits of two
// arguments unread, which is a property of the generated file.
`include "mosaic_id_pkg.svh"
/* verilator lint_on MODDUP */
/* verilator lint_on UNUSEDSIGNAL */
/* verilator lint_on UNUSEDPARAM */

localparam int unsigned TB_ROB_INDEX_W = mosaic_id_pkg::MOSAIC_ID_W_ROB_INDEX;
localparam int unsigned TB_ROB_GEN_W   = mosaic_id_pkg::MOSAIC_ID_W_ROB_GEN;
localparam int unsigned TB_UOP_INDEX_W = mosaic_id_pkg::MOSAIC_ID_W_UOP_INDEX;

module mosaic_muldiv_tb (
    input  logic                          clk,
    input  logic                          rst,

    // request
    input  logic                          req_valid,
    output logic                          req_ready,
    input  logic [2:0]                    req_op,
    input  logic                          req_w,
    input  logic [63:0]                   req_a,
    input  logic [63:0]                   req_b,
    input  logic [TB_ROB_INDEX_W-1:0]     req_rob_index,
    input  logic [TB_ROB_GEN_W-1:0]       req_rob_gen,
    input  logic [TB_UOP_INDEX_W-1:0]     req_uop_index,

    // cancel
    input  logic                          flush,

    // result
    output logic                          res_valid,
    input  logic                          res_ready,
    output logic [63:0]                   res_data,
    output logic [TB_ROB_INDEX_W-1:0]     res_rob_index,
    output logic [TB_ROB_GEN_W-1:0]       res_rob_gen,
    output logic [TB_UOP_INDEX_W-1:0]     res_uop_index,

    // status and coverage
    output logic                          o_busy,
    output logic [7:0]                    o_iter,
    output logic [31:0]                   o_accepted_ctr,
    output logic [31:0]                   o_completed_ctr,
    output logic [31:0]                   o_cancelled_ctr,
    output logic [31:0]                   o_killed_res_ctr,

    // the elaborated identity widths, read back from the generated package
    output logic [31:0]                   o_rob_index_w,
    output logic [31:0]                   o_rob_gen_w,
    output logic [31:0]                   o_uop_index_w
);

  // The driver drives the operation as a plain 3-bit number, so the enum cast
  // happens here once rather than in every port connection.
  mosaic_pkg::md_op_e req_op_typed;

  assign req_op_typed = mosaic_pkg::md_op_e'(req_op);

  mosaic_muldiv u_md (
      .clk_i            (clk),
      .rst_i            (rst),

      .req_valid_i      (req_valid),
      .req_ready_o      (req_ready),
      .req_op_i         (req_op_typed),
      .req_w_i          (req_w),
      .req_a_i          (req_a),
      .req_b_i          (req_b),
      .req_rob_index_i  (req_rob_index),
      .req_rob_gen_i    (req_rob_gen),
      .req_uop_index_i  (req_uop_index),

      .flush_i          (flush),

      .res_valid_o      (res_valid),
      .res_ready_i      (res_ready),
      .res_data_o       (res_data),
      .res_rob_index_o  (res_rob_index),
      .res_rob_gen_o    (res_rob_gen),
      .res_uop_index_o  (res_uop_index),

      .o_busy           (o_busy),
      .o_iter           (o_iter),
      .o_accepted_ctr   (o_accepted_ctr),
      .o_completed_ctr  (o_completed_ctr),
      .o_cancelled_ctr  (o_cancelled_ctr),
      .o_killed_res_ctr (o_killed_res_ctr)
  );

  assign o_rob_index_w = 32'(TB_ROB_INDEX_W);
  assign o_rob_gen_w   = 32'(TB_ROB_GEN_W);
  assign o_uop_index_w = 32'(TB_UOP_INDEX_W);

endmodule : mosaic_muldiv_tb

`resetall
`default_nettype wire
