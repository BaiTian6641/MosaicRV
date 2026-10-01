// ============================================================================
// mosaic_branch_cmp -- work package I-011.
//
// The BEQ/BNE/BLT/BGE/BLTU/BGEU comparison, delivered next to
// `mosaic_branch_target` because that unit takes the comparison result as an
// input (`branch_taken`) and therefore does not compute it. One implementation,
// one place to get the signed/unsigned rule right, and one place a negative
// control can break it.
//
// Contract
// --------
//   * The inputs are register *values*, not register numbers. x0 is the register
//     file's responsibility and it presents 0 here, so a comparison against x0
//     reads as a comparison against zero. This unit cannot tell x0 from any other
//     register that happens to hold 0, and must not try.
//   * The six conditions are the RV64I funct3 encodings: BEQ=000, BNE=001,
//     BLT=100, BGE=101, BLTU=110, BGEU=111. funct3 = 010 and 011 name no
//     condition; the decoder reports them illegal, and this unit answers 0 rather
//     than leaving a don't-care that a later integration could come to depend on.
//   * BLT/BGE are **signed** and BLTU/BGEU are **unsigned**. The pair (-1, 1)
//     separates them: blt(-1,1) is 0 and bltu(-1,1) is 0 as well, but with
//     rs1 = 0 (x0) against rs2 = -1, blt answers 0 while bltu answers 1 -- and
//     bgeu(-1,1) is 1 while bge(0,-1) is 1 too, so the sign-crossing cases are
//     the ones the testbench has to carry.
//   * Purely combinational; no clock, no state.
//
// Negative control
// ----------------
//   `MOSAIC_BRANCH_CMP_MUTANT_1` makes the unsigned conditions use the signed
//   relation. It is off unless that -D is passed, and is demonstrated to fail
//   CASE=alu.boundaries.
//

`default_nettype none

module mosaic_branch_cmp #(
    parameter int XLEN = 64
) (
    input  wire [XLEN-1:0] rs1_value,  // the architectural value, x0 already 0
    input  wire [XLEN-1:0] rs2_value,
    input  wire [2:0]       branch_funct,
    output logic             taken
);
  // RV64I branch funct3. 3'b010 and 3'b011 name no condition.
  localparam logic [2:0] BEQ  = 3'b000;
  localparam logic [2:0] BNE  = 3'b001;
  localparam logic [2:0] BLT  = 3'b100;
  localparam logic [2:0] BGE  = 3'b101;
  localparam logic [2:0] BLTU = 3'b110;
  localparam logic [2:0] BGEU = 3'b111;

  always_comb begin
    case (branch_funct)
      BEQ:  taken = (rs1_value == rs2_value);
      BNE:  taken = (rs1_value != rs2_value);
      BLT:  taken = ($signed(rs1_value) < $signed(rs2_value));
      BGE:  taken = ($signed(rs1_value) >= $signed(rs2_value));
`ifdef MOSAIC_BRANCH_CMP_MUTANT_1
      // NEGATIVE CONTROL: the unsigned conditions use the signed relation, so
      // bltu(-1, 1) is false and bgeu(0, -1) is true.
      BLTU: taken = ($signed(rs1_value) < $signed(rs2_value));
      BGEU: taken = ($signed(rs1_value) >= $signed(rs2_value));
`else
      BLTU: taken = ($unsigned(rs1_value) < $unsigned(rs2_value));
      BGEU: taken = ($unsigned(rs1_value) >= $unsigned(rs2_value));
`endif
      // funct3 = 010 and 011 name no condition. The decoder reports them illegal,
      // so this is a defined answer for a defined encoding rather than a
      // don't-care that a later integration could depend on.
      default: taken = 1'b0;
    endcase
  end

endmodule : mosaic_branch_cmp

`default_nettype wire
