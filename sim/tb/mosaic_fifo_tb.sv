// Simulation wrapper for CASE=fifo.backpressure (work package I-005).
//
// Built and driven by the Verilator harness; see tools/run_unit.py.
// One top module, five independent elastic buffers under a common clock and
// reset:
//
//   d1  mosaic_fifo DEPTH=1   -- the degenerate one-entry case, which must not
//                                 rely on a zero-width pointer wrap
//   d2  mosaic_fifo DEPTH=2   -- power of two
//   d3  mosaic_fifo DEPTH=3   -- NOT a power of two; the pointer is 2 bits wide
//                                 but only values 0..2 are ever reachable, so a
//                                 naive `ptr + 1` wrap would alias
//   d8  mosaic_fifo DEPTH=8   -- power of two, three pointer bits
//   sk  mosaic_skid_buffer    -- one-deep fall-through elastic buffer
//
// Every instance gets its own stimulus and observation ports so the C++ driver
// (sim/unit/tb_fifo.cpp) can drive all five independently in the same cycle and
// therefore cross-check them against each other as well as against the shadow
// model. Instances are driven with different patterns per cycle, so a bug that
// only shows at a particular depth cannot hide behind the others.
//
// There is deliberately no clock generation, no reset generation and no
// `$display` in here: the C++ side owns the clock, the reset schedule and all
// result reporting, per sim/common/sim_common.h.

`default_nettype none

module mosaic_fifo_tb (
    input  wire  logic         clk,
    input  wire  logic         rst,

    // ---- mosaic_fifo DEPTH=1 -------------------------------------------------
    input  wire  logic         d1_in_valid,
    output wire  logic         d1_in_ready,
    input  wire  logic [31:0]  d1_in_payload,
    output wire  logic         d1_out_valid,
    input  wire  logic         d1_out_ready,
    output wire  logic [31:0]  d1_out_payload,
    output wire  logic [0:0]   d1_count,

    // ---- mosaic_fifo DEPTH=2 -------------------------------------------------
    input  wire  logic         d2_in_valid,
    output wire  logic         d2_in_ready,
    input  wire  logic [31:0]  d2_in_payload,
    output wire  logic         d2_out_valid,
    input  wire  logic         d2_out_ready,
    output wire  logic [31:0]  d2_out_payload,
    output wire  logic [1:0]   d2_count,

    // ---- mosaic_fifo DEPTH=3 -------------------------------------------------
    input  wire  logic         d3_in_valid,
    output wire  logic         d3_in_ready,
    input  wire  logic [31:0]  d3_in_payload,
    output wire  logic         d3_out_valid,
    input  wire  logic         d3_out_ready,
    output wire  logic [31:0]  d3_out_payload,
    output wire  logic [1:0]   d3_count,

    // ---- mosaic_fifo DEPTH=8 -------------------------------------------------
    input  wire  logic         d8_in_valid,
    output wire  logic         d8_in_ready,
    input  wire  logic [31:0]  d8_in_payload,
    output wire  logic         d8_out_valid,
    input  wire  logic         d8_out_ready,
    output wire  logic [31:0]  d8_out_payload,
    output wire  logic [3:0]   d8_count,

    // ---- mosaic_skid_buffer --------------------------------------------------
    input  wire  logic         sk_in_valid,
    output wire  logic         sk_in_ready,
    input  wire  logic [31:0]  sk_in_payload,
    output wire  logic         sk_out_valid,
    input  wire  logic         sk_out_ready,
    output wire  logic [31:0]  sk_out_payload
);

  mosaic_fifo #(
      .WIDTH(32),
      .DEPTH(1)
  ) u_d1 (
      .clk        (clk),
      .rst        (rst),
      .in_valid   (d1_in_valid),
      .in_ready   (d1_in_ready),
      .in_payload (d1_in_payload),
      .out_valid  (d1_out_valid),
      .out_ready  (d1_out_ready),
      .out_payload(d1_out_payload),
      .count      (d1_count)
  );

  mosaic_fifo #(
      .WIDTH(32),
      .DEPTH(2)
  ) u_d2 (
      .clk        (clk),
      .rst        (rst),
      .in_valid   (d2_in_valid),
      .in_ready   (d2_in_ready),
      .in_payload (d2_in_payload),
      .out_valid  (d2_out_valid),
      .out_ready  (d2_out_ready),
      .out_payload(d2_out_payload),
      .count      (d2_count)
  );

  mosaic_fifo #(
      .WIDTH(32),
      .DEPTH(3)
  ) u_d3 (
      .clk        (clk),
      .rst        (rst),
      .in_valid   (d3_in_valid),
      .in_ready   (d3_in_ready),
      .in_payload (d3_in_payload),
      .out_valid  (d3_out_valid),
      .out_ready  (d3_out_ready),
      .out_payload(d3_out_payload),
      .count      (d3_count)
  );

  mosaic_fifo #(
      .WIDTH(32),
      .DEPTH(8)
  ) u_d8 (
      .clk        (clk),
      .rst        (rst),
      .in_valid   (d8_in_valid),
      .in_ready   (d8_in_ready),
      .in_payload (d8_in_payload),
      .out_valid  (d8_out_valid),
      .out_ready  (d8_out_ready),
      .out_payload(d8_out_payload),
      .count      (d8_count)
  );

  mosaic_skid_buffer #(
      .WIDTH(32)
  ) u_sk (
      .clk        (clk),
      .rst        (rst),
      .in_valid   (sk_in_valid),
      .in_ready   (sk_in_ready),
      .in_payload (sk_in_payload),
      .out_valid  (sk_out_valid),
      .out_ready  (sk_out_ready),
      .out_payload(sk_out_payload)
  );

endmodule

`default_nettype wire
