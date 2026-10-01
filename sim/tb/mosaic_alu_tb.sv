// ============================================================================
// Simulation wrapper for CASE=alu.boundaries (work package I-011).
//
// Built and driven by the Verilator harness; see tools/run_unit.py.
//
// There is nothing to wrap. `mosaic_alu` is purely combinational -- no clock, no
// reset, no state -- so this top is a straight port-for-port pass-through of the
// instance. It exists rather than making `mosaic_alu` the top for two reasons:
//
//   * the C++ driver (sim/unit/tb_alu.cpp) then talks to a *fixed* pin list that
//     no RTL change can move under it, the same way every other case in this
//     repository is driven;
//   * the wrapper is where a second instance could be added (a different XLEN,
//     or a same-input/different-op pair for a cycle-accurate observation) without
//     touching the unit under test.
//
// Because there is no clock, the driver settles each vector with one Verilator
// eval() and samples the outputs directly; there is no reset to schedule and no
// cycle in which the outputs are invalid. `--max-cycles` therefore bounds the
// number of stimulus vectors applied, not a clock count.
//

`default_nettype none

module mosaic_alu_tb (
    input  wire  [63:0] a,
    input  wire  [63:0] b,
    input  wire  [3:0]  op,       // mosaic_pkg::alu_op_e encoding
    output wire  [63:0] result,
    output wire         zero
);

  mosaic_alu #(
      .XLEN(64)
  ) dut (
      .a      (a),
      .b      (b),
      .op     (op),
      .result (result),
      .zero   (zero)
  );

endmodule : mosaic_alu_tb

`default_nettype wire
