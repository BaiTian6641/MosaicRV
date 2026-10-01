// ============================================================================
// mosaic_ram_tb -- unit-test wrapper for the I-006 synchronous RAM contract.
//
// This is simulation-only glue. It instantiates two independent mosaic_ram
// banks and drives both of them from the same C++ stimulus, with bank B's
// address stream permuted by a fixed XOR. That buys three things at once:
//
//   * the "simultaneous writes to two different addresses" case, because the
//     two write ports are always addressed differently in the same cycle;
//   * the same-address collision case, because a XOR is a bijection and so
//     raddr == waddr in bank B exactly when it is in bank A;
//   * two independent shadows, so a defect that corrupts one bank cannot hide
//     behind the other.
//
// It contains no behaviour of its own: every port is either driven by the C++
// testbench or driven straight out of a DUT output.
// ============================================================================

`default_nettype none
`resetall

module mosaic_ram_tb (
  input  logic        clk,
  input  logic        rst,

  input  logic [5:0]  waddr,
  input  logic [63:0] wdata,
  input  logic [7:0]  wmask,
  input  logic        we,

  input  logic [5:0]  raddr,

  output logic [63:0] rdata_a,
  output logic [63:0] rdata_b,
  output logic        rvalid_a,
  output logic        rvalid_b
);

  // Geometry. sim/unit/tb_ram.cpp hard-codes the same numbers; the generate
  // block below turns a disagreement into an elaboration error rather than a
  // test that silently checks the wrong number of addresses.
  localparam int DATA_WIDTH = 64;
  localparam int DEPTH      = 64;
  localparam int NUM_BYTES  = 8;
  localparam int ADDR_WIDTH = 6;

  // Fixed address permutation applied to bank B. Non-zero so that bank B never
  // addresses the same location as bank A in the same cycle.
  localparam logic [ADDR_WIDTH-1:0] BANK_B_XOR = 6'h2a;

  logic [ADDR_WIDTH-1:0] waddr_b;
  logic [ADDR_WIDTH-1:0] raddr_b;

  assign waddr_b = waddr ^ BANK_B_XOR;
  assign raddr_b = raddr ^ BANK_B_XOR;

  mosaic_ram #(
    .DATA_WIDTH        (DATA_WIDTH),
    .DEPTH             (DEPTH),
    .BYTE_ENABLE_PORTS (1)
  ) u_bank_a (
    .clk    (clk),
    .rst    (rst),
    .waddr  (waddr),
    .wdata  (wdata),
    .wmask  (wmask),
    .we     (we),
    .raddr  (raddr),
    .rdata  (rdata_a),
    .rvalid (rvalid_a)
  );

  mosaic_ram #(
    .DATA_WIDTH        (DATA_WIDTH),
    .DEPTH             (DEPTH),
    .BYTE_ENABLE_PORTS (1)
  ) u_bank_b (
    .clk    (clk),
    .rst    (rst),
    .waddr  (waddr_b),
    .wdata  (wdata),
    .wmask  (wmask),
    .we     (we),
    .raddr  (raddr_b),
    .rdata  (rdata_b),
    .rvalid (rvalid_b)
  );

  // Elaboration guard against the C++ shadow model's geometry.
  generate
    if ((DEPTH != 64) || (DATA_WIDTH != 64) || (NUM_BYTES != 8) ||
        (ADDR_WIDTH != 6)) begin : g_geometry_mismatch
      mosaic_ram_tb_geometry_mismatch u_geometry();
    end
  endgenerate

endmodule

`resetall
