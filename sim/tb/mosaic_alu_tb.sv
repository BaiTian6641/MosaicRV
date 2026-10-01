// ============================================================================
// Simulation wrapper for CASE=alu.boundaries (work package I-011).
//
// Built and driven by the Verilator harness; see tools/run_unit.py.
//
// There is nothing to wrap. Every unit under test is purely combinational -- no
// clock, no reset, no state -- so this top is a straight port-for-port
// pass-through of each instance. It exists rather than making `mosaic_alu` the
// top for two reasons:
//
//   * the C++ driver (sim/unit/tb_alu.cpp) then talks to a *fixed* pin list that
//     no RTL change can move under it, the same way every other case in this
//     repository is driven;
//   * the wrapper is where a second instance could be added (a different XLEN, or
//     a same-input/different-op pair for a cycle-accurate observation) without
//     touching the unit under test.
//
// It carries two groups of pins:
//
//   * the ALU group (`a`, `b`, `op` -> `result`, `zero`);
//   * the branch group (`bt_*` -> `bt_link`, `bt_target`, `bt_is_taken`), which
//     includes the comparator's operands. The comparator lives in RTL next to
//     `mosaic_branch_target` and feeds the `branch_taken` input, so the C++ side
//     checks the comparison and the target arithmetic in the same vector and can
//     tell a wrong condition apart from a wrong address.
//
// Because there is no clock, the driver settles each vector with one Verilator
// eval() and samples the outputs directly; there is no reset to schedule and no
// cycle in which the outputs are invalid. `--max-cycles` therefore bounds the
// number of stimulus vectors applied, not a clock count.
//

`default_nettype none

module mosaic_alu_tb (
    // ---- ALU ---------------------------------------------------------------
    input  wire  [63:0] a,
    input  wire  [63:0] b,
    input  wire  [3:0]  op,       // mosaic_pkg::alu_op_e encoding
    output wire  [63:0] result,
    output wire         zero,

    // ---- branch target -----------------------------------------------------
    input  wire  [63:0] bt_pc,
    input  wire  [63:0] bt_imm,       // already sign-extended by the decoder
    input  wire         bt_is_jalr,
    input  wire  [2:0]  bt_branch_funct,
    input  wire         bt_branch_taken,
    output wire  [63:0] bt_link,
    output wire  [63:0] bt_target,
    output wire         bt_is_taken,

    // ---- branch comparator -------------------------------------------------
    input  wire  [63:0] bt_rs1_value, // architectural value; x0 presents 0
    input  wire  [63:0] bt_rs2_value,
    output wire         bt_cmp_taken
);

  mosaic_alu #(
      .XLEN(64)
  ) u_alu (
      .a      (a),
      .b      (b),
      .op     (op),
      .result (result),
      .zero   (zero)
  );

  mosaic_branch_target #(
      .XLEN(64)
  ) u_branch_target (
      .pc           (bt_pc),
      .imm          (bt_imm),
      .is_jalr      (bt_is_jalr),
      .branch_funct (bt_branch_funct),
      .branch_taken (bt_branch_taken),
      .link         (bt_link),
      .target       (bt_target),
      .is_taken     (bt_is_taken)
  );

  mosaic_branch_cmp #(
      .XLEN(64)
  ) u_branch_cmp (
      .rs1_value   (bt_rs1_value),
      .rs2_value   (bt_rs2_value),
      .branch_funct(bt_branch_funct),
      .taken       (bt_cmp_taken)
  );

endmodule : mosaic_alu_tb

`default_nettype wire
