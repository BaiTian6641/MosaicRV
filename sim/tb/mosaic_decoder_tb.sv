// Simulation wrapper for CASE=decode.rv64im_reserved (work package I-010).
//
// `mosaic_decoder` is purely combinational: one instruction word in, one
// `decode_ctl_t` out, no clock and no reset. This wrapper therefore has no
// clock or reset either -- adding ports the DUT never reads would only create
// unused-signal warnings and a false impression that the decoder has state.
// "One cycle" for this case means one instruction word evaluated, which is what
// sim/unit/tb_decoder.cpp counts.
//
// The struct is broken out into one named port per field rather than passed to
// C++ as a 131-bit opaque vector, so the driver can compare field by field and
// name the field that mismatched. The break-out is a straight member-by-member
// copy and is checked against the DUT's own struct by `RebuildStruct()` in the
// driver, so a typo here cannot hide a decoder bug.
//
// There is deliberately no clock generation, no reset generation and no
// `$display` in here: the C++ side owns the schedule and all result reporting,
// per sim/common/sim_common.h.

`default_nettype none

module mosaic_decoder_tb (
    input  logic [31:0]  insn,

    // decode_ctl_t.valid .. decode_ctl_t.illegal
    output logic         o_valid,
    output logic         o_illegal,

    // operand sources and destinations
    output logic         o_uses_rs1,
    output logic         o_uses_rs2,
    output logic         o_uses_imm,
    output logic [4:0]   o_rs1,
    output logic [4:0]   o_rs2,
    output logic [4:0]   o_rd,
    output logic [63:0]  o_imm,

    // ALU
    output logic [3:0]   o_alu_op,
    output logic         o_uses_alu,
    output logic         o_reg_write,

    // memory
    output logic [2:0]   o_mem_kind,
    output logic [2:0]   o_mem_size,
    output logic         o_mem_signed,

    // control transfer
    output logic         o_is_branch,
    output logic [2:0]   o_branch_funct,
    output logic         o_is_jal,
    output logic         o_is_jalr,
    output logic         o_is_auipc,
    output logic         o_writes_link,

    // misc-mem
    output logic         o_is_miscmem,
    output logic         o_is_fence_i,

    // M extension
    output logic         o_is_muldiv,
    output logic [2:0]   o_md_op,
    output logic         o_md_signed,

    // system
    output logic         o_is_system,
    output logic         o_is_ecall,
    output logic         o_is_ebreak,
    output logic         o_is_mret,
    output logic [1:0]   o_csr_op,
    output logic [11:0]  o_csr_addr,
    output logic         o_csr_writes,
    output logic         o_csr_reads,
    output logic         o_csr_imm_form,

    // The struct itself, as one vector, so the driver can prove that the named
    // ports above carry exactly the bits of decode_ctl_t and that none of the
    // break-out assignments was fat-fingered. Field order is the declaration
    // order of mosaic_pkg::decode_ctl_t, with `valid` at the MSB.
    output logic [132:0] o_ctl_bits
);
  mosaic_pkg::decode_ctl_t ctl;

  assign o_ctl_bits = ctl;

  mosaic_decoder u_dec (
      .insn (insn),
      .ctl  (ctl)
  );

  assign o_valid        = ctl.valid;
  assign o_illegal      = ctl.illegal;
  assign o_uses_rs1     = ctl.uses_rs1;
  assign o_uses_rs2     = ctl.uses_rs2;
  assign o_uses_imm     = ctl.uses_imm;
  assign o_rs1          = ctl.rs1;
  assign o_rs2          = ctl.rs2;
  assign o_rd           = ctl.rd;
  assign o_imm          = ctl.imm;
  assign o_alu_op       = ctl.alu_op;
  assign o_uses_alu     = ctl.uses_alu;
  assign o_reg_write    = ctl.reg_write;
  assign o_mem_kind     = ctl.mem_kind;
  assign o_mem_size     = ctl.mem_size;
  assign o_mem_signed   = ctl.mem_signed;
  assign o_is_branch    = ctl.is_branch;
  assign o_branch_funct = ctl.branch_funct;
  assign o_is_jal       = ctl.is_jal;
  assign o_is_jalr      = ctl.is_jalr;
  assign o_is_auipc     = ctl.is_auipc;
  assign o_writes_link  = ctl.writes_link;
  assign o_is_miscmem   = ctl.is_miscmem;
  assign o_is_fence_i   = ctl.is_fence_i;
  assign o_is_muldiv    = ctl.is_muldiv;
  assign o_md_op        = ctl.md_op;
  assign o_md_signed    = ctl.md_signed;
  assign o_is_system    = ctl.is_system;
  assign o_is_ecall     = ctl.is_ecall;
  assign o_is_ebreak    = ctl.is_ebreak;
  assign o_is_mret      = ctl.is_mret;
  assign o_csr_op       = ctl.csr_op;
  assign o_csr_addr     = ctl.csr_addr;
  assign o_csr_writes   = ctl.csr_writes;
  assign o_csr_reads    = ctl.csr_reads;
  assign o_csr_imm_form = ctl.csr_imm_form;

endmodule

`default_nettype wire
