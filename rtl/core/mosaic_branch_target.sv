// ============================================================================
// mosaic_branch_target -- work package I-011.
//
// The other half of the integer-FU work: the link value a jump writes, the
// address control transfers to, and the predicate that says whether it does.
// Purely combinational -- no clock, no reset, no state.
//
// Its companion `mosaic_branch_cmp` lives in the file next to this one and
// supplies the BEQ/BNE/BLT/BGE/BLTU/BGEU comparison that arrives here as the
// `branch_taken` input.
//
// Contract
// --------
//   * `link` is `pc + 4`, always, for every instruction and every alignment. In
//     IALIGN=32 a jump target that is not 4-byte aligned traps before fetch, so
//     no "aligned link" rule exists and none is invented here.
//   * `target` is the address control transfers to when `is_taken`, and `pc + 4`
//     when it is not. A not-taken branch therefore needs no separate "sequential
//     next PC" path anywhere else in the core: this port is always safe to use.
//   * `JALR` clears bit 0 of the computed target and **nothing else**. Bit 1 is
//     not cleared: the misaligned-target *report* (instruction-address-misaligned
//     or instruction-access-fault, `EXC_INSN_MISALIGNED` / `EXC_INSN_ACCESS`) is
//     the core's job, at the fetch boundary, where the original PC and the
//     original instruction are still available to build tval. A target unit that
//     quietly rounded bit 1 away would hide a trap from the checker.
//   * The immediate arrives already sign-extended by the decoder; this unit never
//     re-sign-extends it, so there is no second place where a W immediate could be
//     widened.
//
// `is_taken` and `branch_funct`
// ----------------------------
//   `is_taken` is high for JALR, for a branch whose comparator said yes, and
//   whenever `branch_funct` names no RV64I condition at all. The branch funct3
//   encodings are BEQ=000, BNE=001, BLT=100, BGE=101, BLTU=110, BGEU=111;
//   funct3 = 010 and 011 name no condition, and the decoder marks them illegal.
//   The last term exists because this unit's port list has `is_jalr` but no
//   `is_jal`, so an unconditional JAL is otherwise indistinguishable from a
//   branch that did not fire. A caller that additionally holds `branch_taken`
//   high for JAL gets the same answer, and the two terms are ORed so that
//   neither convention has to be honoured: the term can only ever make an
//   unconditional transfer look taken, never make a taken branch look untaken.
//
// Negative controls
// -----------------
//   * `MOSAIC_BRANCH_TARGET_MUTANT_1` -- JALR forgets to clear bit 0.
//   * `MOSAIC_BRANCH_TARGET_MUTANT_2` -- `link` is `pc + 2` instead of `pc + 4`.
//   * `MOSAIC_BRANCH_TARGET_MUTANT_3` -- a not-taken branch returns the branch
//     target instead of `pc + 4`.
//

`default_nettype none

module mosaic_branch_target #(
    parameter int XLEN = 64
) (
    input  wire [XLEN-1:0] pc,
    input  wire [XLEN-1:0] imm,         // already sign-extended by the decoder
    input  wire         is_jalr,       // JALR clears bit 0 of the target
    input  wire [2:0]   branch_funct,  // BEQ/BNE/BLT/BGE/BLTU/BGEU
    input  wire         branch_taken,  // the comparison result
    output logic [XLEN-1:0] link,      // PC + 4, for JAL/JALR
    output logic [XLEN-1:0] target,    // the address the PC will take
    output logic            is_taken   // does control actually transfer
);
  // RV64I branch funct3. 3'b010 and 3'b011 name no condition.
  localparam logic [2:0] BR_NONE_A = 3'b010;
  localparam logic [2:0] BR_NONE_B = 3'b011;

  logic [XLEN-1:0] sum;      // pc + imm
  logic [XLEN-1:0] aligned; // the transfer target, with JALR's bit 0 cleared
  logic            names_condition;

  always_comb begin
`ifdef MOSAIC_BRANCH_TARGET_MUTANT_2
    // NEGATIVE CONTROL: the wrong link distance.
    link = pc + {{(XLEN - 3){1'b0}}, 3'd2};
`else
    link = pc + {{(XLEN - 3){1'b0}}, 3'd4};
`endif

    sum = pc + imm;

`ifdef MOSAIC_BRANCH_TARGET_MUTANT_1
    // NEGATIVE CONTROL: JALR forgets that bit 0 of the target is always zero.
    aligned = sum;
`else
    aligned = is_jalr ? {sum[XLEN-1:1], 1'b0} : sum;
`endif

    names_condition = (branch_funct != BR_NONE_A) && (branch_funct != BR_NONE_B);
    is_taken        = is_jalr | ~names_condition | branch_taken;

`ifdef MOSAIC_BRANCH_TARGET_MUTANT_3
    // NEGATIVE CONTROL: a not-taken branch leaks the branch target, so the
    // sequential next PC depends on the branch resolving correctly.
    target = aligned;
`else
    // A not-taken branch yields pc + 4, which is exactly `link`.
    target = is_taken ? aligned : link;
`endif
  end

endmodule : mosaic_branch_target

`default_nettype wire
