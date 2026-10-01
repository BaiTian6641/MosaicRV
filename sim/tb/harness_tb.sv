// Top-level wrapper for the harness testbench (I-004).
//
// It is a pure adapter: the harness drives the probe over a stepping interface
// and performs every memory access itself, so memory faults, the cycle limit
// and the event stream all live in one place. The wrapper exists only to give
// the simulator one named top-level module.

`default_nettype none

module harness_tb (
    input  wire         clk,
    input  wire         rst,

    input  wire         step_valid,
    output wire         step_ready,
    input  wire  [63:0] pc_in,
    input  wire  [31:0] insn_in,

    output wire         mem_en,
    output wire  [63:0] mem_addr,
    output wire  [63:0] mem_wdata,
    output wire  [7:0]  mem_byte_en,

    output wire         retire,
    output wire  [63:0] next_pc,
    output wire         illegal,

    output wire         rd_we,
    output wire  [4:0]  rd_index,
    output wire  [63:0] rd_value_out
);

  harness_probe u_probe (
      .clk         (clk),
      .rst         (rst),
      .step_valid  (step_valid),
      .step_ready  (step_ready),
      .pc_in       (pc_in),
      .insn_in     (insn_in),
      .mem_en      (mem_en),
      .mem_addr    (mem_addr),
      .mem_wdata   (mem_wdata),
      .mem_byte_en (mem_byte_en),
      .retire      (retire),
      .next_pc     (next_pc),
      .illegal     (illegal),
      .rd_we       (rd_we),
      .rd_index    (rd_index),
      .rd_value_out(rd_value_out)
  );

endmodule

`default_nettype wire
