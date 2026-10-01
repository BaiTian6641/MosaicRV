// ============================================================================
// mosaic_result_fifo_tb -- unit-test wrapper for CASE=completion.fu_collision,
// work package I-025.
//
// This is simulation-only glue and it contains no behaviour of its own: every
// port is driven by sim/unit/tb_result_fifo.cpp or comes straight out of the
// DUT. There is no clock generation, no reset generation and no `$display` in
// here; the C++ side owns the clock, the reset schedule and all result
// reporting, per sim/common/sim_common.h.
//
// Widths are derived from the generated configuration package, so a profile that
// changed a knob changes this wrapper with it. That include is safe *because*
// `mosaic_cfg_pkg.svh` carries an include guard (it did not, once, and four RTL
// files including it was how the omission was found). `mosaic_id_pkg.svh` does
// NOT carry one, so it is deliberately not included here: `mosaic_result_fifo.sv`
// is the single place in this compilation unit that may include it, and the two
// identity widths this wrapper needs are expressed from the configuration
// numbers instead. The DUT's own widths are read back out of the elaborated
// instance onto `o_dut_*` and compared against the wrapper's `o_tb_*` by the
// driver, so if that re-expression ever disagreed with the generated identity
// package the case would fail loudly instead of silently narrowing a port.
// ============================================================================

`default_nettype none
`resetall

/* verilator lint_off UNUSEDPARAM */
// The generated header declares one localparam per knob for the whole project;
// this wrapper names the execution-fabric subset and the rest are unused here.
`include "mosaic_cfg_pkg.svh"
/* verilator lint_on UNUSEDPARAM */

localparam int unsigned TB_ENTRIES     = mosaic_cfg_pkg::MOSAIC_RESULT_FIFO;
localparam int unsigned TB_XLEN        = mosaic_cfg_pkg::MOSAIC_XLEN;
localparam int unsigned TB_ROB_INDEX_W = mosaic_cfg_pkg::MOSAIC_ROB_INDEX_W;
// The ROB generation is one bit wider than the ROB index: see mosaic_id_pkg's
// MOSAIC_ID_W_ROB_GEN, whose value this re-expresses from the same geometry.
localparam int unsigned TB_ROB_GEN_W   = $clog2(mosaic_cfg_pkg::MOSAIC_ROB_ENTRIES) + 1;
localparam int unsigned TB_UOP_W       = $clog2(mosaic_cfg_pkg::MOSAIC_MAX_UOPS_PER_MACRO);

// One producer port per completing source in the cluster: the ALUs of this
// cluster, plus the shared MUL/DIV and the LSU, which complete into the cluster
// that owns the uop.
localparam int unsigned TB_PRODUCERS   =
    mosaic_cfg_pkg::MOSAIC_ALU_PER_CLUSTER + mosaic_cfg_pkg::MOSAIC_MULDIV_UNITS +
    mosaic_cfg_pkg::MOSAIC_LSU_UNITS;

localparam int unsigned TB_ENTRY_W =
    TB_ROB_INDEX_W + TB_ROB_GEN_W + TB_UOP_W + TB_XLEN + 1 + TB_XLEN + TB_XLEN;
localparam int unsigned TB_CNT_W = $clog2(TB_ENTRIES + 1);

module mosaic_result_fifo_tb (
    input  logic clk,
    input  logic rst,

    // producers: valid/ready/payload, one payload word per producer, producer 0
    // in the least significant slice
    input  logic [TB_PRODUCERS-1:0]            p_valid,
    output logic [TB_PRODUCERS-1:0]            p_ready,
    input  logic [TB_PRODUCERS*TB_ENTRY_W-1:0] p_pay,

    // consumer
    output logic                               c_valid,
    input  logic                               c_ready,
    output logic [TB_ENTRY_W-1:0]              c_pay,

    // kill
    input  logic                               kill_valid,
    input  logic                               kill_all,
    input  logic [TB_ROB_INDEX_W-1:0]          kill_rob_index,
    input  logic [TB_ROB_GEN_W-1:0]            kill_rob_gen,

    // status and observation
    output logic [TB_CNT_W-1:0]                o_count,
    output logic [TB_ENTRIES-1:0]              o_occ,
    output logic [TB_ENTRIES-1:0]              o_exc_occ,
    output logic [31:0]                        o_push_ctr,
    output logic [31:0]                        o_pop_ctr,
    output logic [31:0]                        o_kill_ctr,
    output logic [31:0]                        o_kill_offer_ctr,
    output logic [31:0]                        o_kill_total,
    output logic [TB_ENTRIES*TB_ENTRY_W-1:0]   o_entry_pay,

    // ------------------------------------------------------------ geometry
    // What the DUT elaborated, read out of the instance hierarchy: the driver
    // sizes its shadow from these, so it contains no geometry of its own.
    output logic [31:0] o_dut_entries,
    output logic [31:0] o_dut_producers,
    output logic [31:0] o_dut_entry_w,
    output logic [31:0] o_dut_cnt_w,
    output logic [31:0] o_dut_xlen,
    output logic [31:0] o_dut_rob_index_w,
    output logic [31:0] o_dut_rob_gen_w,
    output logic [31:0] o_dut_uop_w,

    // What this wrapper was built with. The driver requires that the two sets
    // agree field by field, which is what makes a profile change a build failure
    // rather than a silently narrowed port.
    output logic [31:0] o_tb_entries,
    output logic [31:0] o_tb_producers,
    output logic [31:0] o_tb_entry_w,
    output logic [31:0] o_tb_cnt_w,
    output logic [31:0] o_tb_xlen,
    output logic [31:0] o_tb_rob_index_w,
    output logic [31:0] o_tb_rob_gen_w,
    output logic [31:0] o_tb_uop_w
);

  mosaic_result_fifo u_dut (
      .clk              (clk),
      .rst              (rst),
      .p_valid          (p_valid),
      .p_ready          (p_ready),
      .p_pay            (p_pay),
      .c_valid          (c_valid),
      .c_ready          (c_ready),
      .c_pay            (c_pay),
      .kill_valid       (kill_valid),
      .kill_all         (kill_all),
      .kill_rob_index   (kill_rob_index),
      .kill_rob_gen     (kill_rob_gen),
      .o_count          (o_count),
      .o_occ            (o_occ),
      .o_exc_occ        (o_exc_occ),
      .o_push_ctr       (o_push_ctr),
      .o_pop_ctr        (o_pop_ctr),
      .o_kill_ctr       (o_kill_ctr),
      .o_kill_offer_ctr (o_kill_offer_ctr),
      .o_kill_total     (o_kill_total),
      .o_entry_pay      (o_entry_pay)
  );

  assign o_dut_entries     = 32'(u_dut.ENTRIES);
  assign o_dut_producers   = 32'(u_dut.PRODUCERS);
  assign o_dut_entry_w     = 32'(u_dut.ENTRY_W);
  assign o_dut_cnt_w       = 32'(u_dut.CNT_W);
  assign o_dut_xlen        = 32'(u_dut.XLEN);
  assign o_dut_rob_index_w = 32'(u_dut.ROB_INDEX_W);
  assign o_dut_rob_gen_w   = 32'(u_dut.ROB_GEN_W);
  assign o_dut_uop_w       = 32'(u_dut.UOP_W);

  assign o_tb_entries     = 32'(TB_ENTRIES);
  assign o_tb_producers   = 32'(TB_PRODUCERS);
  assign o_tb_entry_w     = 32'(TB_ENTRY_W);
  assign o_tb_cnt_w       = 32'(TB_CNT_W);
  assign o_tb_xlen        = 32'(TB_XLEN);
  assign o_tb_rob_index_w = 32'(TB_ROB_INDEX_W);
  assign o_tb_rob_gen_w   = 32'(TB_ROB_GEN_W);
  assign o_tb_uop_w       = 32'(TB_UOP_W);

endmodule : mosaic_result_fifo_tb

`resetall
`default_nettype wire
