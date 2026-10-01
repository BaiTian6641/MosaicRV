// Simulation wrapper for CASE=prf.read_bank_collision (work package I-015).
//
// `mosaic_prf` is a stateful single-clock module with combinational read
// responses (a demand offered in cycle N is answered in cycle N). This wrapper
// adds no timing of its own: it passes every port straight through.
//
// It does three things of its own, and each is about not being a second source
// of truth:
//
//   * Every port width is **derived from the generated packages**, not written
//     out by hand. A hand-written "96 entries" or "7-bit tag" in a testbench is
//     a second copy of the geometry that a profile change would silently
//     contradict, and a mismatch there shows up as a slice of a vector the driver
//     never looks at -- i.e. as a test that got weaker without failing. The port
//     lists are connected by name below, so a profile that changed either side
//     fails to build on a width mismatch rather than quietly narrowing an
//     observation the driver reads.
//
//   * The whole storage is exposed for comparison (`dbg_valid`, `dbg_gen`,
//     `dbg_data`). The DUT's ports cannot show a write that lands in the wrong
//     row of the right bank and is never read, and that is exactly the class of
//     defect a banked register file has. The arrays are reached hierarchically
//     because the register file's interface is frozen and carries no observation
//     ports -- adding one would be a change to the contract under test, made by
//     the test.
//
//   * The elaborated geometry is read back out as outputs. The driver sizes its
//     own shadow from those and from nothing else, so `sim/unit/tb_prf.cpp`
//     contains no depth, no tag width, no bank count and no row count, and its
//     first checks after reset fail if the numbers do not describe a coherent
//     register file.
//
// There is deliberately no clock generation, no reset generation and no
// `$display` in here: the C++ side owns the clock, the reset schedule and all
// result reporting, per sim/common/sim_common.h.

`default_nettype none
`resetall

/* verilator lint_off UNUSEDPARAM */
// The generated headers declare one localparam per configuration knob for the
// whole project. This wrapper needs the register-file subset; the rest belong to
// other modules and are unused *here* by construction, not by omission.
//
// Both headers carry their own include guards, and `mosaic_id_pkg.svh` silences
// the unused-signal warnings its own project-wide helpers raise, so neither an
// include guard nor a duplicate-package waiver is needed here.
`include "mosaic_cfg_pkg.svh"
`include "mosaic_id_pkg.svh"
/* verilator lint_on UNUSEDPARAM */

localparam int unsigned TB_ENTRIES = mosaic_cfg_pkg::MOSAIC_INT_PRF_ENTRIES;
localparam int unsigned TB_TAG_W   = mosaic_cfg_pkg::MOSAIC_INT_PRF_TAG_W;
localparam int unsigned TB_BANKS   = mosaic_cfg_pkg::MOSAIC_PRF_BANKS;
localparam int unsigned TB_BANK_W  = (TB_BANKS > 1) ? $clog2(TB_BANKS) : 1;
localparam int unsigned TB_ROWS    = (TB_ENTRIES + TB_BANKS - 1) / TB_BANKS;
localparam int unsigned TB_ROW_W   = (TB_ROWS > 1) ? $clog2(TB_ROWS) : 1;
localparam int unsigned TB_GEN_W   = mosaic_id_pkg::MOSAIC_ID_W_PRF_GEN;
localparam int unsigned TB_XLEN    = mosaic_cfg_pkg::MOSAIC_XLEN;
// Storage slots, which is not the same number as entries when the bank count
// does not divide the entry count: the surplus rows exist and must be observable,
// or a write that aliased into one of them would be invisible.
localparam int unsigned TB_SLOTS   = TB_BANKS * TB_ROWS;

module mosaic_prf_tb (
    input  logic                              clk,
    input  logic                              rst,

    // one write port per bank
    input  logic [TB_BANKS-1:0]               wr_en_i,
    input  logic [TB_BANKS-1:0]               wr_gen_valid_i,
    input  logic [TB_BANKS*TB_TAG_W-1:0]      wr_tag_i,
    input  logic [TB_BANKS*TB_GEN_W-1:0]      wr_gen_i,
    input  logic [TB_BANKS*TB_XLEN-1:0]       wr_data_i,

    // one read demand per bank
    input  logic [TB_BANKS-1:0]               rd_valid_i,
    output logic [TB_BANKS-1:0]               rd_ready_o,
    input  logic [TB_BANKS*TB_TAG_W-1:0]      rd_tag_i,
    input  logic [TB_BANKS*TB_GEN_W-1:0]      rd_gen_i,
    output logic [TB_BANKS-1:0]               rsp_valid_o,
    output logic [TB_BANKS*TB_TAG_W-1:0]      rsp_tag_o,
    output logic [TB_BANKS*TB_GEN_W-1:0]      rsp_gen_o,
    output logic [TB_BANKS*TB_XLEN-1:0]       rsp_data_o,
    output logic [TB_BANKS-1:0]               rsp_gen_mismatch_o,
    output logic [TB_BANKS-1:0]               rsp_never_written_o,

    // status
    output logic [31:0]                       o_wr_ctr,
    output logic [31:0]                       o_rd_ctr,
    output logic [31:0]                       o_conflict_ctr,
    output logic [31:0]                       o_mismatch_ctr,
    output logic [31:0]                       o_invalid_ctr,
    output logic                              o_busy,

    // whole-storage observation
    output logic [TB_SLOTS-1:0]               dbg_valid,
    output logic [TB_SLOTS*TB_GEN_W-1:0]      dbg_gen,
    output logic [TB_SLOTS*TB_XLEN-1:0]       dbg_data,

    // the geometry this file was sized from, so the driver can check that the
    // numbers it is about to model are a coherent register file
    output logic [31:0]                       o_entries,
    output logic [31:0]                       o_tag_w,
    output logic [31:0]                       o_gen_w,
    output logic [31:0]                       o_xlen,
    output logic [31:0]                       o_banks,
    output logic [31:0]                       o_bank_w,
    output logic [31:0]                       o_rows,
    output logic [31:0]                       o_row_w
);

  mosaic_prf u_prf (
      .clk_i               (clk),
      .rst_i               (rst),

      .wr_en_i             (wr_en_i),
      .wr_gen_valid_i      (wr_gen_valid_i),
      .wr_tag_i            (wr_tag_i),
      .wr_gen_i            (wr_gen_i),
      .wr_data_i           (wr_data_i),

      .rd_valid_i          (rd_valid_i),
      .rd_ready_o          (rd_ready_o),
      .rd_tag_i            (rd_tag_i),
      .rd_gen_i            (rd_gen_i),
      .rsp_valid_o         (rsp_valid_o),
      .rsp_tag_o           (rsp_tag_o),
      .rsp_gen_o           (rsp_gen_o),
      .rsp_data_o          (rsp_data_o),
      .rsp_gen_mismatch_o  (rsp_gen_mismatch_o),
      .rsp_never_written_o (rsp_never_written_o),

      .o_wr_ctr            (o_wr_ctr),
      .o_rd_ctr            (o_rd_ctr),
      .o_conflict_ctr      (o_conflict_ctr),
      .o_mismatch_ctr      (o_mismatch_ctr),
      .o_invalid_ctr       (o_invalid_ctr),
      .o_busy              (o_busy)
  );

  // The storage, reached hierarchically because the frozen register-file
  // interface has no observation ports. Slot (b, r) is packed at
  // b*TB_ROWS + r so the driver can walk the arrays in the same order the
  // bank/row arithmetic produces, and so a row that no tag can reach is still
  // visible.
  generate
    for (genvar gb = 0; gb < TB_BANKS; gb++) begin : g_bank
      for (genvar gr = 0; gr < TB_ROWS; gr++) begin : g_row
        assign dbg_valid[gb*TB_ROWS + gr] = u_prf.valid_q[gb][gr];
        assign dbg_gen[(gb*TB_ROWS + gr)*TB_GEN_W +: TB_GEN_W] = u_prf.gen_q[gb][gr];
        assign dbg_data[(gb*TB_ROWS + gr)*TB_XLEN +: TB_XLEN] = u_prf.data_q[gb][gr];
      end
    end
  endgenerate

  // The geometry, straight from the generated packages this file already
  // included to size its ports. There is no second copy: the RTL derives the
  // same numbers from the same packages.
  assign o_entries = 32'(TB_ENTRIES);
  assign o_tag_w   = 32'(TB_TAG_W);
  assign o_gen_w   = 32'(TB_GEN_W);
  assign o_xlen    = 32'(TB_XLEN);
  assign o_banks   = 32'(TB_BANKS);
  assign o_bank_w  = 32'(TB_BANK_W);
  assign o_rows    = 32'(TB_ROWS);
  assign o_row_w   = 32'(TB_ROW_W);

endmodule : mosaic_prf_tb

`resetall
`default_nettype wire
