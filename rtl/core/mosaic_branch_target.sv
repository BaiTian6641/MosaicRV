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
//   * `link` is `pc + insn_len`, always: four bytes for a base-ISA instruction,
//     two for a compressed one (I-041). The length is an input because it is a
//     property of the instruction, and inferring it from the PC's alignment
//     would be wrong -- a 32-bit instruction may start at a two-byte-aligned
//     address. In IALIGN=16 the two-byte form is the only other case.
//   * `target` is the address control transfers to when `is_taken`, and the
//     sequential next instruction (`pc + insn_len`) when it is not. A not-taken
//     branch therefore needs no separate "sequential next PC" path anywhere
//     else in the core: this port is always safe to use.
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
// What decides control transfer
// -----------------------------
//   `is_taken = is_jal | is_jalr | (is_branch & branch_taken)`.
//
//   Nothing here sniffs the instruction encoding. An earlier revision of this
//   module was given a port list without `is_branch`/`is_jal`, and inferred "this
//   is an unconditional jump" from a `branch_funct` that named no RV64I condition
//   (funct3 = 010 or 011). That was wrong in kind rather than in degree: it
//   invented semantics for two reserved encodings, so any future instruction
//   whose funct3 collided with the pattern would have had `is_taken` disagree with
//   real control flow, and would have disagreed only for instructions nobody was
//   thinking about. `decode_ctl_t` already carries `is_branch`, `is_jal` and
//   `is_jalr` as separate fields, so the decoder states what the instruction is
//   and this unit does the arithmetic.
//
//   A useful property falls out of the `&`: a caller that leaves `is_branch` low
//   but drives `branch_taken` high still gets `is_taken = 0`, which is the correct
//   answer for an instruction that named no branch. A wrongly wired comparator is
//   therefore loud -- it cannot silently redirect a JAL or an AUIPC.
//
// Negative controls
// -----------------
//   * `MOSAIC_BRANCH_TARGET_MUTANT_1` -- JALR forgets to clear bit 0.
//   * `MOSAIC_BRANCH_TARGET_MUTANT_2` -- `link` is `pc + 2` instead of `pc + 4`.
//   * `MOSAIC_BRANCH_TARGET_MUTANT_3` -- a not-taken branch returns the branch
//     target instead of `pc + 4`.
//   * `MOSAIC_BRANCH_CMP_MUTANT_1`    -- BLTU/BGEU compare with the signed
//     relation instead of the unsigned one (in mosaic_branch_cmp.sv).
//

`default_nettype none

module mosaic_branch_target #(
    parameter int XLEN = 64
) (
    input  wire [XLEN-1:0] pc,
    input  wire [2:0]   insn_len,      // the instruction's own length in bytes
                                        // (4, or 2 for a compressed jump)
    input  wire [XLEN-1:0] imm,        // already sign-extended by the decoder
    input  wire         is_branch,    // a conditional branch: BEQ..BGEU
    input  wire         is_jal,       // unconditional jump with its own immediate
    input  wire         is_jalr,      // unconditional jump, target bit 0 forced 0
    input  wire         branch_taken, // result of mosaic_branch_cmp, or 0
    output logic [XLEN-1:0] link,     // always pc + 4
    output logic [XLEN-1:0] target,   // the address control transfers to
    output logic            is_taken  // does control actually transfer
);
  logic [XLEN-1:0] sum;      // pc + imm
  logic [XLEN-1:0] aligned; // the transfer target, with JALR's bit 0 cleared

  always_comb begin
`ifdef MOSAIC_BRANCH_TARGET_MUTANT_2
    // NEGATIVE CONTROL: the wrong link distance.
    link = pc + {{(XLEN - 3){1'b0}}, 3'd2};
`else
    // The link value is `pc + the instruction's own length`. It was `pc + 4`
    // while every instruction was four bytes; a compressed JALR is two, and its
    // return address is the byte after it. The length is an input rather than a
    // constant precisely because it is a property of the instruction, not of
    // this unit.
    link = pc + {{(XLEN - 3){1'b0}}, insn_len};
`endif

    sum = pc + imm;

`ifdef MOSAIC_BRANCH_TARGET_MUTANT_1
    // NEGATIVE CONTROL: JALR forgets that bit 0 of the target is always zero.
    aligned = sum;
`else
    aligned = is_jalr ? {sum[XLEN-1:1], 1'b0} : sum;
`endif

    is_taken = is_jal | is_jalr | (is_branch & branch_taken);

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
