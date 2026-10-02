// MosaicRV work package I-010 -- `mosaic_decoder`.
//
// Purely combinational RV64IM decoder: one 32-bit instruction in, one
// `mosaic_pkg::decode_ctl_t` out. No clock, no reset, no state. Every field of
// `ctl` is a function of `insn` alone, so the decoder can sit anywhere in the
// front end and its output can be sampled whenever the instruction word is
// stable.
//
// Scope
// -----
//   * RV64I base: lui auipc jal jalr, the six branches, the eight loads and
//     stores, the OP-IMM / OP / OP-IMM-32 forms, fence, fence.i, ecall, ebreak
//     and the six Zicsr register forms.
//   * M extension: mul mulh mulhsu mulhu div divu rem remu, classified into
//     `is_muldiv` / `md_op` / `md_signed` for the muldiv unit (I-012), which
//     owns the datapath and is not instantiated here.
//   * RV64I word arithmetic on OP-32 (0111011): addw subw sllw srlw sraw, on
//     funct7 0000000/0100000. R-type: rs1 and rs2 are registers and the shift
//     amount is rs2[4:0], exactly as on the 64-bit OP forms.
//   * M extension word forms: mulw divw divuw remw remuw, on OP-32 with
//     funct7 0000001, classified like their 64-bit counterparts plus `md_w`.
//     The five encodings the ISA defines are legal; funct3 001/010/011 are
//     reserved because there is no mulhw/mulhsuw/mulhuw.
//   * mret, classified but not executed: the trap unit (I-019) consumes it.
//   * Everything else is illegal. There is no partial decode. An instruction is
//     either legal -- in which case `ctl` is CTL_ILLEGAL with the fields this
//     instruction drives overwritten -- or illegal, in which case `ctl` is
//     exactly CTL_ILLEGAL with nothing stale left over. A reserved encoding
//     that half-works is how a reserved encoding becomes an unexplained bug.
//
// Field contract
// --------------
//   valid / illegal    illegal implies !valid, always.
//   reg_write          already qualified by rd != 0. A write to x0 is not a
//                      register write, so nothing downstream has to special-case
//                      x0 again (I-013 relies on this).
//   rd / rs1 / rs2     the raw instruction fields *wherever those bits are
//                      register fields*; x0 where the bits are an immediate
//                      fragment instead: rs1/rs2 are fragments for U-type
//                      (lui/auipc) and rd is a fragment for S-type and B-type.
//   imm                sign-extended to 64 bits, and the ONLY immediate the
//                      instruction has. `uses_imm` means "imm participates":
//                      the address add for loads and stores, the shift amount
//                      for the six shift-immediates (zero-extended into imm,
//                      never sign-extended), and the control-transfer offset.
//   alu_op / uses_alu  driven only when uses_alu is 1. Control transfers leave
//                      them at the CTL_ILLEGAL values ALU_PASSB / 0 and are
//                      driven by `branch_funct` / `writes_link` instead: the
//                      link value is PC + 4, produced by the branch unit.
//   is_auipc           means ALU operand A is the instruction's PC.
//   uses_rs1 == 0      means ALU operand A is hard-wired zero (lui).
//   md_signed          1 for mul, mulh, mulhsu, div, rem; 0 for mulhu, divu,
//                      remu -- exactly the three funct3 patterns 011/101/111.
//                      mulhsu's mixed signedness is carried by MD_MULHSU
//                      itself, not by this bit.
//   md_w               the word forms carry the same md_op/md_signed encoding
//                      as their 64-bit counterparts and additionally set md_w,
//                      because the datapath has to know to work on the low 32
//                      bits and sign-extend the 32-bit result. Every 64-bit
//                      form leaves it 0, so a consumer that ignores md_w gets
//                      the 64-bit behaviour rather than a half-word one.
//   csr_reads/writes   the architectural intent after the x0 rules: csrrw
//                      with rd == x0 does not read, csrrs/csrrc with rs1 == x0
//                      do not write. `uses_rs1` is 1 for every CSR form -- in
//                      the immediate forms insn[19:15] is a zero-extended
//                      5-bit zimm, not a register index, and `csr_imm_form`
//                      says which.
//   jalr target        NOT computed here. `is_jalr` + `uses_imm` + `imm` is
//                      what the core needs: target = pc + rs1 + imm, after
//                      which the core clears target bit 0. Clearing bit 0 is a
//                      target-computation rule owned by I-011, not an encoding
//                      rule, so it is deliberately not folded into `imm`.
//
// Reserved encodings
// ------------------
// The illegal set is enumerated and asserted by CASE=decode.rv64im_reserved,
// not left to fall out of a default arm.
// results/reports/I-010-011-decode-alu.md explains each reserved class.
//
// OP-32 has reserved classes of its own and all of them stay illegal here:
// funct3 001/010/011 under funct7 0000001 (which would name mulhw/mulhsuw/
// mulhuw -- the ISA defines no high-half word multiply), every funct7 other
// than 0000000, 0100000 and 0000001, and the funct3 patterns those two I-word
// funct7 values do not define (they carry only 000/001/101 under 0000000 and
// 000/101 under 0100000).
//
// An earlier revision of this file decoded the *whole* of OP-32 as illegal and
// called the five RV64I word forms "RV32-only ... reserved on RV64". That was
// wrong: RV32I defines no OP-32 at all, so `addw` and friends are RV64
// instructions and a core that traps them breaks any compiler that emits them.
// results/reports/I-010-011-decode-alu.md carries the correction and the
// objdump evidence; MOSAIC_DECODER_MUTANT_W_FORMS_ILLEGAL restores the defect so
// that the unit case proves it cannot come back unnoticed.
//
// Mutation hooks
// --------------
// The shipping build defines none of the `MOSAIC_DECODER_MUTANT_*` macros. Each
// injects exactly one broken behaviour so the unit test can be shown to detect
// it; the mutant table is in results/reports/I-010-011-decode-alu.md. They
// exist to prove the test has teeth and have no place in any other build.

`default_nettype none

// Every reference to a package name in this file is either fully qualified
// (`mosaic_pkg::decode_ctl_t`, `mosaic_pkg::md_op_e`) or is covered by an
// in-body `import mosaic_pkg::*;` below. That is deliberate: a qualified
// reference resolves regardless of the order Verilator is given the files, so
// this module lints clean whether the package is listed before or after it.
// A $unit-scope (file-scope) `import *` was tried and is worse on both counts --
// it raises IMPORTSTAR under -Wall, and under -Wall that warning is an error.

module mosaic_decoder (
    // `insn_raw` is the instruction window the fetch unit delivered. For a
    // 32-bit instruction the whole word is the encoding; for a 16-bit
    // compressed instruction only `insn_raw[15:0]` is, and `insn16` says
    // which it is. The compressed forms are expanded to their base-ISA
    // equivalents below, so every arm of the decode -- and every consumer of
    // `ctl` -- is the same code it was before C existed.
    input  wire  [31:0]             insn_raw,
    input  wire                     insn16,
    output mosaic_pkg::decode_ctl_t ctl
);

  import mosaic_pkg::*;

  // ---------------------------------------------------------------- mutants
`ifdef MOSAIC_DECODER_MUTANT_SRAI_FUNCT3
  localparam bit MutSraiFunct3 = 1'b1;   // srai accepted with funct3 != 101
`else
  localparam bit MutSraiFunct3 = 1'b0;
`endif
`ifdef MOSAIC_DECODER_MUTANT_S_IMM_AS_I
  localparam bit MutSImmAsI    = 1'b1;   // S immediate built with I layout
`else
  localparam bit MutSImmAsI    = 1'b0;
`endif
`ifdef MOSAIC_DECODER_MUTANT_W_FORMS_ILLEGAL
  localparam bit MutWIFormsIllegal = 1'b1;  // the five RV64I OP-32 W arithmetic
                                            // forms rejected again
`else
  localparam bit MutWIFormsIllegal = 1'b0;
`endif
`ifdef MOSAIC_DECODER_MUTANT_RESERVED_F3_LEGAL
  localparam bit MutReservedF3 = 1'b1;   // LOAD funct3 111 accepted
`else
  localparam bit MutReservedF3 = 1'b0;
`endif
`ifdef MOSAIC_DECODER_MUTANT_LWU_ILLEGAL
  localparam bit MutLwuIllegal = 1'b1;   // LWU (LOAD funct3 110) refused as illegal,
                                         // the defect V-013 reported
`else
  localparam bit MutLwuIllegal = 1'b0;
`endif
`ifdef MOSAIC_DECODER_MUTANT_SHIFT_UPPER_IGNORED
  localparam bit MutShiftUpper = 1'b1;   // slli ignores insn[31:26]
`else
  localparam bit MutShiftUpper = 1'b0;
`endif
`ifdef MOSAIC_DECODER_MUTANT_B_IMM_SWAPPED
  localparam bit MutBImmSwap   = 1'b1;   // B immediate halves exchanged
`else
  localparam bit MutBImmSwap   = 1'b0;
`endif
`ifdef MOSAIC_DECODER_MUTANT_CSR_INTENT
  localparam bit MutCsrIntent  = 1'b1;   // CSR read/write intent ignores x0
`else
  localparam bit MutCsrIntent  = 1'b0;
`endif
`ifdef MOSAIC_DECODER_MUTANT_C_RESERVED_EXEC
  localparam bit MutCReservedExec = 1'b1;  // a reserved compressed encoding is
                                           // expanded and executed instead of
                                           // being refused as illegal (I-041)
`else
  localparam bit MutCReservedExec = 1'b0;
`endif
`ifdef MOSAIC_DECODER_MUTANT_C_LUI_IMM
  localparam bit MutCLuiImm    = 1'b1;   // c.lui's 6-bit immediate placed 12 bits
                                         // too high in rd (0x5000000 for `c.lui
                                         // x1, 5`), the defect V-043 reported
`else
  localparam bit MutCLuiImm    = 1'b0;
`endif

  // ==========================================================================
  // C (compressed) decompression -- work package I-041
  // ==========================================================================
  // A 16-bit instruction is *expanded* to the base-ISA instruction it is
  // shorthand for, and the existing 32-bit decode then runs unchanged. That is
  // the whole layering decision: there is one decoder, one `decode_ctl_t` and
  // one meaning for every field, and nothing downstream of this module knows
  // the C extension exists. The alternative -- a second control-word producer
  // for 16-bit forms -- is the "second opinion about what the machine does"
  // this file's header already refuses.
  //
  // Three outcomes per encoding, and each is stated rather than implied:
  //
  //   * an expansion exists (the encoding is defined RV64C) -> that word is
  //     decoded, `ctl.valid` is set, and the instruction executes;
  //   * the encoding is reserved -> `ok` is 0 and the compressed arm drives
  //     nothing, so `ctl` is exactly CTL_ILLEGAL (never a half-decoded word);
  //   * the encoding is a defined HINT (c.nop, c.addi x0, c.li x0, c.lui x0,
  //     c.slli x0, c.srli/c.srai/c.andi x0, c.mv x0, c.add x0) -> it expands to
  //     an instruction whose destination is x0, so it is *legal* and writes
  //     nothing. A hint that trapped would be a wrong machine.
  //
  // The F and D extension's compressed forms (c.fld, c.fsd, c.fldsp, c.fsdsp)
  // are refused as illegal because p0 has no floating point; that is an
  // unimplemented extension, not a reserved encoding, and the report for I-041
  // says so.
  //
  // `force` is the mutation hook: when MutCReservedExec is set the reserved
  // tests are skipped, so a reserved encoding is expanded "as if" and executes
  // -- the exact defect CASE=compressed.cross_boundary's third control injects.
  typedef struct packed {
    logic        ok;     // legal compressed encoding (reserved -> 0)
    logic [31:0] word;   // the equivalent base-ISA instruction
  } cexp_t;

  // The compressed register fields name x8..x15.
  function automatic logic [4:0] c_creg(input logic [2:0] field);
    c_creg = {2'b01, field};
  endfunction

  // Base-ISA instruction builders: each takes the fields its format defines and
  // places them exactly where the ISA puts them, so an expansion cannot be
  // "almost right" about one bit position.
  function automatic logic [31:0] rv_r(input logic [6:0] f7, input logic [4:0] rs2,
                                       input logic [4:0] rs1, input logic [2:0] f3,
                                       input logic [4:0] rd,  input logic [6:0] op);
    rv_r = {f7, rs2, rs1, f3, rd, op};
  endfunction

  function automatic logic [31:0] rv_i(input logic [11:0] imm, input logic [4:0] rs1,
                                       input logic [2:0] f3,   input logic [4:0] rd,
                                       input logic [6:0] op);
    rv_i = {imm, rs1, f3, rd, op};
  endfunction

  function automatic logic [31:0] rv_s(input logic [11:0] imm, input logic [4:0] rs2,
                                       input logic [4:0] rs1,  input logic [2:0] f3,
                                       input logic [6:0] op);
    rv_s = {imm[11:5], rs2, rs1, f3, imm[4:0], op};
  endfunction

  // B/J formats always have an even immediate: their bit 0 is not encoded and
  // is hard-wired zero, so the builders take bits [12:1] / [20:1] and there is
  // no variable bit that is silently never read.
  function automatic logic [31:0] rv_b(input logic [12:1] imm, input logic [4:0] rs2,
                                       input logic [4:0] rs1,  input logic [2:0] f3,
                                       input logic [6:0] op);
    rv_b = {imm[12], imm[10:5], rs2, rs1, f3, imm[4:1], imm[11], op};
  endfunction

  function automatic logic [31:0] rv_u(input logic [19:0] imm, input logic [4:0] rd,
                                       input logic [6:0] op);
    rv_u = {imm, rd, op};
  endfunction

  function automatic logic [31:0] rv_j(input logic [20:1] imm, input logic [4:0] rd,
                                       input logic [6:0] op);
    rv_j = {imm[20], imm[10:1], imm[11], imm[19:12], rd, op};
  endfunction

  // The expansion itself: quadrant, then funct3, then the sub-encodings whose
  // meaning is a second field. Each `ok` assignment is one of the reserved
  // cases the RV64C text names.
  function automatic cexp_t c_expand(input logic [15:0] c, input logic force_reserved);
    cexp_t       r;
    logic [2:0]  cf3;
    logic [4:0]  rdp, rs1p, rs2p;
    logic [11:0] imm12;
    logic [12:1] imm13;
    logic [20:1] imm21;
    logic [19:0] imm20;
    // The CI-format immediate (c.addi, c.addiw, c.li, c.andi): imm[5] is the
    // encoding's bit 12 and imm[4:0] are its bits 6:2, sign-extended to 12 bits.
    // The register field sits between the two halves, so the immediate is *not*
    // a slice of the instruction: taking `c[12:7]` reads the destination
    // register as the low immediate bits, which is what CASE=compressed.
    // cross_boundary's reference caught (a `c.addi x6, 1` that added 6).
    logic [11:0] ci_imm;

    ci_imm = {{6{c[12]}}, c[12], c[6:2]};

    r.ok   = 1'b1;
    r.word = rv_i(12'd0, 5'd0, mosaic_pkg::F3_ADD_SUB, 5'd0, mosaic_pkg::OP_IMM);

    cf3  = c[15:13];
    rdp  = c_creg(c[4:2]);
    rs1p = c_creg(c[9:7]);
    rs2p = c_creg(c[4:2]);

    case (c[1:0])
      // ------------------------------------------------------- quadrant 0
      2'b00: begin
        case (cf3)
          3'b000: begin  // c.addi4spn: addi rd', x2, nzuimm
            imm12  = {2'b00, c[10:7], c[12:11], c[5], c[6], 2'b00};
            r.ok   = force_reserved || (imm12 != 12'd0);   // nzuimm == 0 reserved
            r.word = rv_i(imm12, 5'd2, mosaic_pkg::F3_ADD_SUB, rdp, mosaic_pkg::OP_IMM);
          end
          3'b001, 3'b101: r.ok = 1'b0;  // c.fld / c.fsd: no D extension in p0
          3'b010: begin  // c.lw: lw rd', uimm(rs1')
            imm12  = {5'b00000, c[5], c[12:10], c[6], 2'b00};
            r.word = rv_i(imm12, rs1p, 3'b010, rdp, mosaic_pkg::OP_LOAD);
          end
          3'b011: begin  // c.ld: ld rd', uimm(rs1')
            imm12  = {4'b0000, c[6:5], c[12:10], 3'b000};
            r.word = rv_i(imm12, rs1p, 3'b011, rdp, mosaic_pkg::OP_LOAD);
          end
          3'b100: r.ok = 1'b0;  // reserved
          3'b110: begin  // c.sw: sw rs2', uimm(rs1')
            imm12  = {5'b00000, c[5], c[12:10], c[6], 2'b00};
            r.word = rv_s(imm12, rs2p, rs1p, 3'b010, mosaic_pkg::OP_STORE);
          end
          3'b111: begin  // c.sd: sd rs2', uimm(rs1')
            imm12  = {4'b0000, c[6:5], c[12:10], 3'b000};
            r.word = rv_s(imm12, rs2p, rs1p, 3'b011, mosaic_pkg::OP_STORE);
          end
          default: r.ok = 1'b0;
        endcase
      end

      // ------------------------------------------------------- quadrant 1
      2'b01: begin
        case (cf3)
          3'b000: begin  // c.nop (rd=0, imm=0) / c.addi: addi rd, rd, imm
            imm12  = ci_imm;
            r.word = rv_i(imm12, c[11:7], mosaic_pkg::F3_ADD_SUB, c[11:7],
                          mosaic_pkg::OP_IMM);
          end
          3'b001: begin  // c.addiw (RV64): addiw rd, rd, imm
            imm12  = ci_imm;
            r.ok   = force_reserved || (c[11:7] != 5'd0);   // rd == x0 reserved
            r.word = rv_i(imm12, c[11:7], mosaic_pkg::F3_ADD_SUB, c[11:7],
                          mosaic_pkg::OP_IMM_32);
          end
          3'b010: begin  // c.li: addi rd, x0, imm (rd == x0 is a hint)
            imm12  = ci_imm;
            r.word = rv_i(imm12, 5'd0, mosaic_pkg::F3_ADD_SUB, c[11:7],
                          mosaic_pkg::OP_IMM);
          end
          3'b011: begin
            if (c[11:7] == 5'd2) begin  // c.addi16sp: addi x2, x2, nzimm
              imm12  = {{2{c[12]}}, c[12], c[4:3], c[5], c[2], c[6], 4'b0000};
              r.ok   = force_reserved || (imm12 != 12'd0);   // nzimm == 0 reserved
              r.word = rv_i(imm12, 5'd2, mosaic_pkg::F3_ADD_SUB, 5'd2,
                            mosaic_pkg::OP_IMM);
            end else begin  // c.lui: lui rd, nzimm (rd == x0 a hint, imm == 0 reserved)
              // The C.LUI immediate is the 6-bit nzimm -- encoding bit 12 is
              // nzimm[5] and encoding bits 6:2 are nzimm[4:0] -- sign-extended to
              // 20 bits and placed at imm20[5:0]; `lui rd, imm20` then puts it at
              // rd[17:12], which is where the ISA defines it. Building imm20 as
              // if the field sat at imm20[16:12] (and padding the bottom with
              // 12'b0) shifts the result twelve bits too high: `c.lui x1, 5`
              // produced 0x5000000 instead of 0x5000. CASE=core.act_dut's
              // Zca-c.lui-00 ELF is the case that pins this.
              imm20  = MutCLuiImm ? {{2{c[12]}}, c[12], c[6:2], 12'b0}
                                  : {{14{c[12]}}, c[12], c[6:2]};
              r.ok   = force_reserved || (c[12] || (c[6:2] != 5'd0));
              r.word = rv_u(imm20, c[11:7], 7'b0110111);
            end
          end
          3'b100: begin
            case (c[11:10])
              2'b00: begin  // c.srli
                imm12  = {6'b000000, c[12], c[6:2]};
                r.word = rv_i(imm12, rs1p, mosaic_pkg::F3_SRL_SRA, rs1p,
                              mosaic_pkg::OP_IMM);
              end
              2'b01: begin  // c.srai
                imm12  = {6'b010000, c[12], c[6:2]};
                r.word = rv_i(imm12, rs1p, mosaic_pkg::F3_SRL_SRA, rs1p,
                              mosaic_pkg::OP_IMM);
              end
              2'b10: begin  // c.andi
                imm12  = ci_imm;
                r.word = rv_i(imm12, rs1p, mosaic_pkg::F3_AND, rs1p,
                              mosaic_pkg::OP_IMM);
              end
              default: begin
                if (!c[12]) begin
                  case (c[6:5])
                    2'b00: r.word = rv_r(7'b0100000, rs2p, rs1p, 3'b000, rs1p,
                                         mosaic_pkg::OP_MUL_DIV);  // c.sub
                    2'b01: r.word = rv_r(7'b0000000, rs2p, rs1p, 3'b100, rs1p,
                                         mosaic_pkg::OP_MUL_DIV);  // c.xor
                    2'b10: r.word = rv_r(7'b0000000, rs2p, rs1p, 3'b110, rs1p,
                                         mosaic_pkg::OP_MUL_DIV);  // c.or
                    default: r.word = rv_r(7'b0000000, rs2p, rs1p, 3'b111, rs1p,
                                           mosaic_pkg::OP_MUL_DIV);  // c.and
                  endcase
                end else begin
                  case (c[6:5])
                    2'b00: r.word = rv_r(7'b0100000, rs2p, rs1p, 3'b000, rs1p,
                                         mosaic_pkg::OP_32);  // c.subw
                    2'b01: r.word = rv_r(7'b0000000, rs2p, rs1p, 3'b000, rs1p,
                                         mosaic_pkg::OP_32);  // c.addw
                    default: r.ok = 1'b0;   // reserved
                  endcase
                end
              end
            endcase
          end
          3'b101: begin  // c.j: jal x0, offset
            imm21  = {{9{c[12]}}, c[12], c[8], c[10:9], c[6], c[7], c[2], c[11],
                      c[5:3]};
            r.word = rv_j(imm21, 5'd0, mosaic_pkg::OP_JAL);
          end
          3'b110: begin  // c.beqz: beq rs1', x0, offset
            imm13  = {{4{c[12]}}, c[12], c[6:5], c[2], c[11:10], c[4:3]};
            r.word = rv_b(imm13, 5'd0, rs1p, 3'b000, mosaic_pkg::OP_BRANCH);
          end
          default: begin  // c.bnez: bne rs1', x0, offset
            imm13  = {{4{c[12]}}, c[12], c[6:5], c[2], c[11:10], c[4:3]};
            r.word = rv_b(imm13, 5'd0, rs1p, 3'b001, mosaic_pkg::OP_BRANCH);
          end
        endcase
      end

      // ------------------------------------------------------- quadrant 2
      2'b10: begin
        case (cf3)
          3'b000: begin  // c.slli (rd == x0 is a hint)
            imm12  = {6'b000000, c[12], c[6:2]};
            r.word = rv_i(imm12, c[11:7], mosaic_pkg::F3_SLL, c[11:7],
                          mosaic_pkg::OP_IMM);
          end
          3'b001, 3'b101: r.ok = 1'b0;  // c.fldsp / c.fsdsp: no D extension
          3'b010: begin  // c.lwsp: lw rd, uimm(x2)
            imm12  = {4'b0000, c[3:2], c[12], c[6:4], 2'b00};
            r.ok   = force_reserved || (c[11:7] != 5'd0);   // rd == x0 reserved
            r.word = rv_i(imm12, 5'd2, 3'b010, c[11:7], mosaic_pkg::OP_LOAD);
          end
          3'b011: begin  // c.ldsp: ld rd, uimm(x2)
            imm12  = {3'b000, c[4:2], c[12], c[6:5], 3'b000};
            r.ok   = force_reserved || (c[11:7] != 5'd0);   // rd == x0 reserved
            r.word = rv_i(imm12, 5'd2, 3'b011, c[11:7], mosaic_pkg::OP_LOAD);
          end
          3'b100: begin
            if (!c[12]) begin
              if (c[6:2] == 5'd0) begin  // c.jr: jalr x0, 0(rs1)
                r.ok   = force_reserved || (c[11:7] != 5'd0);  // rs1 == x0 reserved
                r.word = rv_i(12'd0, c[11:7], 3'b000, 5'd0, mosaic_pkg::OP_JALR);
              end else begin  // c.mv: add rd, x0, rs2 (rd == x0 a hint)
                r.word = rv_r(7'b0000000, c[6:2], 5'd0, 3'b000, c[11:7],
                              mosaic_pkg::OP_MUL_DIV);
              end
            end else if (c[6:2] == 5'd0) begin
              if (c[11:7] == 5'd0) begin  // c.ebreak
                r.word = 32'h0010_0073;
              end else begin  // c.jalr: jalr x1, 0(rs1)
                r.word = rv_i(12'd0, c[11:7], 3'b000, 5'd1, mosaic_pkg::OP_JALR);
              end
            end else begin  // c.add: add rd, rd, rs2 (rd == x0 a hint)
              r.word = rv_r(7'b0000000, c[6:2], c[11:7], 3'b000, c[11:7],
                            mosaic_pkg::OP_MUL_DIV);
            end
          end
          3'b110: begin  // c.swsp: sw rs2, uimm(x2)
            imm12  = {4'b0000, c[8:7], c[12:9], 2'b00};
            r.word = rv_s(imm12, c[6:2], 5'd2, 3'b010, mosaic_pkg::OP_STORE);
          end
          default: begin  // c.sdsp: sd rs2, uimm(x2)
            imm12  = {3'b000, c[9:7], c[12:10], 3'b000};
            r.word = rv_s(imm12, c[6:2], 5'd2, 3'b011, mosaic_pkg::OP_STORE);
          end
        endcase
      end

      default: r.ok = 1'b0;
    endcase
    return r;
  endfunction

  // The word every arm below decodes. For a 32-bit instruction it is the input
  // window itself; for a 16-bit one it is the expansion, and `expand_ok` is the
  // expansion's own statement that the encoding is not reserved.
  function automatic cexp_t c_word(input logic [31:0] raw, input logic is16);
    cexp_t r;
    if (is16) begin
      r = c_expand(raw[15:0], MutCReservedExec);
    end else begin
      r.ok   = 1'b1;
      r.word = raw;
    end
    return r;
  endfunction

  logic [31:0] insn;
  logic        expand_ok;

  always_comb begin
    cexp_t e;
    e = c_word(insn_raw, insn16);
    insn      = e.word;
    expand_ok = e.ok;
  end

  // ------------------------------------------------------- field extraction
  localparam logic [6:0] F7_BASE = 7'b0000000;  // the "first" of a funct3 pair
  localparam logic [6:0] F7_ALT  = 7'b0100000;  // the "second" (sub, sra)
  localparam logic [6:0] F7_M    = 7'b0000001;  // M extension marker

  logic [6:0] opcode;
  logic [2:0] funct3;
  logic [6:0] funct7;
  logic [4:0] rd_f;
  logic [4:0] rs1_f;
  logic [4:0] rs2_f;

  assign opcode = insn[6:0];
  assign funct3 = insn[14:12];
  assign funct7 = insn[31:25];
  assign rd_f   = insn[11:7];
  assign rs1_f  = insn[19:15];
  assign rs2_f  = insn[24:20];

  // Sign extension helpers, each taking exactly the number of immediate bits
  // the format defines, so a mis-sized concatenation is a width error at lint
  // rather than a silent lost sign bit at simulation.
  function automatic logic [63:0] sext12(input logic [11:0] value);
    sext12 = {{52{value[11]}}, value};
  endfunction

  function automatic logic [63:0] sext13(input logic [12:0] value);
    sext13 = {{51{value[12]}}, value};
  endfunction

  function automatic logic [63:0] sext21(input logic [20:0] value);
    sext21 = {{43{value[20]}}, value};
  endfunction

  function automatic logic [63:0] sext32(input logic [31:0] value);
    sext32 = {{32{value[31]}}, value};
  endfunction

  // ------------------------------------------------------------- immediates
  // I-type: imm[11:0] = insn[31:20]. insn[11:7] is rd here -- S-type is the
  // format that borrows those bits for the low half of its immediate, and
  // using this layout for a store silently corrupts every address whose
  // offset needs bits 4:0.
  logic [63:0] imm_i;
  // S-type: imm[11:5] = insn[31:25], imm[4:0] = insn[11:7].
  logic [63:0] imm_s;
  // B-type: imm[12] insn[31], imm[11] insn[7], imm[10:5] insn[30:25],
  //         imm[4:1] insn[11:8], imm[0] hard-wired zero.
  logic [12:0] b_raw;
  logic [63:0] imm_b;
  // U-type: imm[31:12] = insn[31:12], imm[11:0] hard-wired zero.
  logic [63:0] imm_u;
  // J-type: imm[20] insn[31], imm[10:1] insn[30:21], imm[11] insn[20],
  //         imm[19:12] insn[19:12], imm[0] hard-wired zero.
  logic [20:0] j_raw;
  logic [63:0] imm_j;

  assign imm_i = sext12(insn[31:20]);
  assign imm_s = MutSImmAsI ? sext12(insn[31:20])
                            : sext12({insn[31:25], insn[11:7]});
  assign b_raw = MutBImmSwap ? {insn[31], insn[7], insn[11:8], insn[30:25], 1'b0}
                             : {insn[31], insn[7], insn[30:25], insn[11:8], 1'b0};
  assign imm_b = sext13(b_raw);
  assign imm_u = sext32({insn[31:12], 12'h000});
  assign j_raw = {insn[31], insn[19:12], insn[20], insn[30:21], 1'b0};
  assign imm_j = sext21(j_raw);

  // Shift amounts. The RV64 shift-immediates carry a 6-bit shamt in insn[25:20]
  // and are therefore selected by insn[31:26]; the *word* shift-immediates
  // carry a 5-bit shamt in insn[24:20] and are selected by insn[31:25]. Using
  // the 7-bit funct7 field for the 64-bit forms is the classic off-by-one-bit
  // bug: it rejects every shamt whose insn[31:26] is not zero, i.e. shamt 32
  // to 63, all of which are legal.
  logic [5:0] shamt_x;
  logic [4:0] shamt_w;
  logic [63:0] imm_slli;   // shift amount, zero-extended, never sign-extended

  assign shamt_x  = insn[25:20];
  assign shamt_w  = insn[24:20];
  assign imm_slli = MutShiftUpper ? {52'd0, insn[31:20]} : {58'd0, shamt_x};

  // M-extension signedness: the unsigned operations are mulhu (011), divu (101)
  // and remu (111). Written out, because "all three bits set" is wrong -- that
  // is 111 only, and would classify mulhu and divu as signed.
  logic md_is_signed;
  assign md_is_signed = (funct3 != 3'b011) && (funct3 != 3'b101) &&
                        (funct3 != 3'b111);

  // The one and only illegal state. Every field is a defined constant, so an
  // illegal encoding cannot leave a stale rd or a stale mem_size behind, and
  // "the decoder drove nothing" is a single named value rather than a list of
  // defaults that can drift apart.

  localparam mosaic_pkg::decode_ctl_t CTL_ILLEGAL = '{
      valid:        1'b0,
      illegal:      1'b1,
      alu_op:       mosaic_pkg::ALU_PASSB,   // defined "no ALU operation" encoding
      md_op:        mosaic_pkg::MD_MUL,
      mem_kind:     mosaic_pkg::MEM_NONE,
      mem_size:     mosaic_pkg::SZ_BYTE,
      csr_op:       mosaic_pkg::CSR_NONE,
      // `amo_op` is an enum, so it cannot be covered by the `'0` default arm
      // (an implicit bit-to-enum conversion is rejected); it is named
      // explicitly and left at the operation the encoding space starts with.
      amo_op:       mosaic_pkg::AMO_ADD,
      default:      '0
  };

  always_comb begin : decode
    // `legal` is the single source of truth for valid/illegal: an arm that
    // forgets to set it produces a fully illegal ctl, not a half-decoded one.
    bit legal;

    ctl  = CTL_ILLEGAL;
    legal = 1'b0;

    // A reserved compressed encoding has no base-ISA equivalent at all, so it
    // drives *nothing*: `legal` stays low and the fixup below emits exactly
    // CTL_ILLEGAL. Note the order -- the expansion's own `ok` is consulted
    // before the 32-bit case, not after, so a reserved encoding cannot be
    // "decoded anyway" by whatever word the expansion happened to leave behind.
    if (insn16 && !expand_ok) begin
      legal = 1'b0;
    end else case (opcode)
      // ------------------------------------------------------------- loads
      mosaic_pkg::OP_LOAD: begin
        ctl.uses_rs1  = 1'b1;
        ctl.uses_imm  = 1'b1;
        ctl.rs1       = rs1_f;
        ctl.rd        = rd_f;
        ctl.imm       = imm_i;
        ctl.reg_write = (rd_f != 5'd0);
        ctl.mem_kind  = mosaic_pkg::MEM_LOAD;
        ctl.uses_alu  = 1'b1;
        ctl.alu_op    = mosaic_pkg::ALU_ADD;   // effective address = rs1 + imm
        legal         = 1'b1;
        case (funct3)
          mosaic_pkg::F3_ADD_SUB: begin ctl.mem_size = mosaic_pkg::SZ_BYTE; ctl.mem_signed = 1'b1; end  // lb
          mosaic_pkg::F3_SLL:     begin ctl.mem_size = mosaic_pkg::SZ_HALF; ctl.mem_signed = 1'b1; end  // lh
          mosaic_pkg::F3_SLT:     begin ctl.mem_size = mosaic_pkg::SZ_WORD; ctl.mem_signed = 1'b1; end  // lw
          mosaic_pkg::F3_SLTU:    begin ctl.mem_size = mosaic_pkg::SZ_DBL;  ctl.mem_signed = 1'b1; end  // ld
          mosaic_pkg::F3_XOR:     begin ctl.mem_size = mosaic_pkg::SZ_BYTE; ctl.mem_signed = 1'b0; end  // lbu
          3'd5:       begin ctl.mem_size = mosaic_pkg::SZ_HALF; ctl.mem_signed = 1'b0; end  // lhu
          // funct3 110 is LWU, the zero-extending word load RV64I requires:
          // the same 32-bit access as lw with no sign extension into the upper
          // half. Only funct3 111 is reserved -- RV64I has no load wider than ld
          // and no store-by-width encoding beyond the six defined above.
          mosaic_pkg::F3_OR: begin
            // Mutation: go back to refusing LWU as a reserved encoding.
            if (MutLwuIllegal) begin
              legal = 1'b0;
            end else begin
              ctl.mem_size   = mosaic_pkg::SZ_WORD;
              ctl.mem_signed = 1'b0;
            end
          end
          3'd7: begin
            // Mutation: accept a reserved funct3 as a signed doubleword load.
            if (MutReservedF3) begin
              ctl.mem_size   = mosaic_pkg::SZ_DBL;
              ctl.mem_signed = 1'b1;
            end else begin
              legal = 1'b0;
            end
          end
          default: legal = 1'b0;
        endcase
      end

      // -------------------------------------------------- fence / fence.i
      mosaic_pkg::OP_MISC_MEM: begin
        // Only funct3 000 (fence) and 001 (fence.i) are defined. The
        // fm/pred/succ fields of fence are all legal values and are not
        // decoded here: the memory system owns them.
        if ((funct3 == mosaic_pkg::F3_ADD_SUB) || (funct3 == mosaic_pkg::F3_SLL)) begin
          ctl.is_miscmem = 1'b1;              // fence or fence.i
          ctl.is_fence_i = (funct3 == mosaic_pkg::F3_SLL);  // funct3 001 is fence.i
          legal         = 1'b1;
        end
      end

      // ----------------------------------------------------------- OP-IMM
      mosaic_pkg::OP_IMM: begin
        ctl.uses_rs1  = 1'b1;
        ctl.uses_imm  = 1'b1;
        ctl.rs1       = rs1_f;
        ctl.rd        = rd_f;
        ctl.uses_alu  = 1'b1;
        ctl.reg_write = (rd_f != 5'd0);
        ctl.imm       = imm_i;
        legal         = 1'b1;
        case (funct3)
          // Mutation: accept srai with funct3 000. On a real RV64 core
          // funct3 000 is addi whatever insn[31:26] says, so this defect shows
          // up as `addi` decoding as `srai` -- a register write with the wrong
          // operation and the wrong immediate width.
          mosaic_pkg::F3_ADD_SUB: begin
            if (MutSraiFunct3 && (insn[31:26] == 6'h10)) begin
              ctl.alu_op = mosaic_pkg::ALU_SRA;
              ctl.imm    = imm_slli;
            end else begin
              ctl.alu_op = mosaic_pkg::ALU_ADD;   // addi
            end
          end
          mosaic_pkg::F3_SLL: begin                          // slli
            ctl.alu_op = mosaic_pkg::ALU_SLL;
            // Mutation: ignore insn[31:26], so "shift by 64" (shamt field
            // zero with a nonzero upper field) decodes as a legal slli.
            if (MutShiftUpper || (insn[31:26] == 6'h00)) ctl.imm = imm_slli;
            else                                          legal = 1'b0;
          end
          mosaic_pkg::F3_SLT:  ctl.alu_op = mosaic_pkg::ALU_SLT;        // slti
          mosaic_pkg::F3_SLTU: ctl.alu_op = mosaic_pkg::ALU_SLTU;       // sltiu; immediate sign-extended
          mosaic_pkg::F3_XOR:  ctl.alu_op = mosaic_pkg::ALU_XOR;        // xori
          mosaic_pkg::F3_SRL_SRA: begin                       // srli / srai
            // funct3 101 alone does not say which one: insn[31:26] does.
            // Mutation: drop the funct3 101 requirement from the srai arm.
            if (insn[31:26] == 6'h00) begin
              ctl.alu_op = mosaic_pkg::ALU_SRL;
              ctl.imm    = imm_slli;
            end else if ((insn[31:26] == 6'h10) &&
                         (MutSraiFunct3 || (funct3 == mosaic_pkg::F3_SRL_SRA))) begin
              ctl.alu_op = mosaic_pkg::ALU_SRA;
              ctl.imm    = imm_slli;
            end else begin
              legal = 1'b0;
            end
          end
          mosaic_pkg::F3_OR:   ctl.alu_op = mosaic_pkg::ALU_OR;         // ori
          mosaic_pkg::F3_AND:  ctl.alu_op = mosaic_pkg::ALU_AND;        // andi
          default: legal = 1'b0;
        endcase
      end

      // --------------------------------------------------------------- lui
      // U-type with no operands at all: the answer is the immediate itself, so
      // ALU_PASSB is the operation. funct3 is not part of the encoding, and
      // insn[19:15] / insn[24:20] are imm[19:15] / imm[9:5], not registers --
      // which is why rs1 and rs2 stay at x0 here and in auipc.
      7'b0110111: begin
        ctl.uses_imm  = 1'b1;
        ctl.imm       = imm_u;
        ctl.rd        = rd_f;
        ctl.reg_write = (rd_f != 5'd0);
        ctl.uses_alu  = 1'b1;
        ctl.alu_op    = mosaic_pkg::ALU_PASSB;
        legal         = 1'b1;
      end

      // ------------------------------------------------------------- auipc
      // funct3 is not part of the encoding: every value is auipc.
      mosaic_pkg::OP_AUIPC: begin
        ctl.uses_imm  = 1'b1;
        ctl.imm       = imm_u;
        ctl.rd        = rd_f;
        ctl.reg_write = (rd_f != 5'd0);
        ctl.is_auipc  = 1'b1;      // ALU operand A is the PC
        ctl.uses_alu  = 1'b1;
        ctl.alu_op    = mosaic_pkg::ALU_ADD;
        legal         = 1'b1;
      end

      // ------------------------------------------------------------ stores
      mosaic_pkg::OP_STORE: begin
        ctl.uses_rs1 = 1'b1;
        ctl.uses_rs2 = 1'b1;
        ctl.uses_imm = 1'b1;
        ctl.rs1      = rs1_f;
        ctl.rs2      = rs2_f;     // insn[11:7] is imm[4:0], not rd
        ctl.imm      = imm_s;
        ctl.mem_kind = mosaic_pkg::MEM_STORE;
        ctl.uses_alu = 1'b1;
        ctl.alu_op   = mosaic_pkg::ALU_ADD;
        legal        = 1'b1;
        case (funct3)
          mosaic_pkg::F3_ADD_SUB: ctl.mem_size = mosaic_pkg::SZ_BYTE;   // sb
          3'd1:       ctl.mem_size = mosaic_pkg::SZ_HALF;   // sh
          mosaic_pkg::F3_SLT:     ctl.mem_size = mosaic_pkg::SZ_WORD;   // sw
          3'd3:       ctl.mem_size = mosaic_pkg::SZ_DBL;    // sd
          default:    legal = 1'b0;              // funct3 100..111 reserved
        endcase
      end

      // ------------------------------------------------------- OP-IMM-32
      // RV64 only: the 32-bit word forms live on OP-32, which is illegal here.
      mosaic_pkg::OP_IMM_32: begin
        ctl.uses_rs1  = 1'b1;
        ctl.uses_imm  = 1'b1;
        ctl.rs1       = rs1_f;
        ctl.rd        = rd_f;
        ctl.reg_write = (rd_f != 5'd0);
        ctl.uses_alu  = 1'b1;
        ctl.imm       = imm_i;
        legal         = 1'b1;
        case (funct3)
          mosaic_pkg::F3_ADD_SUB: ctl.alu_op = mosaic_pkg::ALU_ADDW;    // addiw
          mosaic_pkg::F3_SLL: begin                          // slliw
            ctl.alu_op = mosaic_pkg::ALU_SLLW;
            if (funct7 == F7_BASE) ctl.imm = {59'd0, shamt_w};
            else                   legal = 1'b0;
          end
          mosaic_pkg::F3_SRL_SRA: begin                       // srliw / sraiw
            if (funct7 == F7_BASE) begin
              ctl.alu_op = mosaic_pkg::ALU_SRLW;
              ctl.imm    = {59'd0, shamt_w};
            end else if (funct7 == F7_ALT) begin
              ctl.alu_op = mosaic_pkg::ALU_SRAW;
              ctl.imm    = {59'd0, shamt_w};
            end else begin
              legal = 1'b0;
            end
          end
          default: legal = 1'b0;
        endcase
      end

      // ------------------------------------------------------- OP and M
      mosaic_pkg::OP_MUL_DIV: begin
        ctl.uses_rs1  = 1'b1;
        ctl.uses_rs2 = 1'b1;
        ctl.rs1       = rs1_f;
        ctl.rs2       = rs2_f;
        ctl.rd        = rd_f;
        ctl.reg_write = (rd_f != 5'd0);
        legal         = 1'b1;
        if (funct7 == F7_M) begin
          // M extension: funct7 == 0000001 selects it, funct3 the operation.
          // The datapath is I-012's; only the classification is here.
          ctl.is_muldiv = 1'b1;
          ctl.md_op     = mosaic_pkg::md_op_e'(funct3);
          ctl.md_signed = md_is_signed;
        end else begin
          ctl.uses_alu = 1'b1;
          case (funct3)
            mosaic_pkg::F3_ADD_SUB: begin
              if (funct7 == F7_BASE)     ctl.alu_op = mosaic_pkg::ALU_ADD;
              else if (funct7 == F7_ALT) ctl.alu_op = mosaic_pkg::ALU_SUB;
              else                          legal = 1'b0;
            end
            mosaic_pkg::F3_SLL: begin
              if (funct7 == F7_BASE)     ctl.alu_op = mosaic_pkg::ALU_SLL;
              else                          legal = 1'b0;
            end
            mosaic_pkg::F3_SLT: begin
              if (funct7 == F7_BASE)     ctl.alu_op = mosaic_pkg::ALU_SLT;
              else                          legal = 1'b0;
            end
            mosaic_pkg::F3_SLTU: begin
              if (funct7 == F7_BASE)     ctl.alu_op = mosaic_pkg::ALU_SLTU;
              else                          legal = 1'b0;
            end
            mosaic_pkg::F3_XOR: begin
              if (funct7 == F7_BASE)     ctl.alu_op = mosaic_pkg::ALU_XOR;
              else                          legal = 1'b0;
            end
            mosaic_pkg::F3_SRL_SRA: begin
              if (funct7 == F7_BASE)     ctl.alu_op = mosaic_pkg::ALU_SRL;
              else if (funct7 == F7_ALT) ctl.alu_op = mosaic_pkg::ALU_SRA;
              else                          legal = 1'b0;
            end
            mosaic_pkg::F3_OR: begin
              if (funct7 == F7_BASE)     ctl.alu_op = mosaic_pkg::ALU_OR;
              else                          legal = 1'b0;
            end
            default: begin
              if (funct7 == F7_BASE)     ctl.alu_op = mosaic_pkg::ALU_AND;
              else                          legal = 1'b0;
            end
          endcase
        end
      end

      // ------------------------------------------------------------ branch
      mosaic_pkg::OP_BRANCH: begin
        ctl.uses_rs1     = 1'b1;
        ctl.uses_rs2     = 1'b1;
        ctl.uses_imm     = 1'b1;
        ctl.rs1          = rs1_f;
        ctl.rs2          = rs2_f;   // insn[11:7] is imm[4:1], not rd
        ctl.imm          = imm_b;
        ctl.is_branch    = 1'b1;
        ctl.branch_funct = funct3;
        // beq 000, bne 001, blt 100, bge 101, bltu 110, bgeu 111. The
        // reserved pair is funct3 010 and 011 -- the branch encoding reuses the
        // funct3 space but not the ALU's meaning of it, so funct3 100 is blt
        // here and not a reserved value.
        if ((funct3 == mosaic_pkg::F3_ADD_SUB) || (funct3 == mosaic_pkg::F3_SLL)  ||
            (funct3 == mosaic_pkg::F3_XOR)     || (funct3 == mosaic_pkg::F3_SRL_SRA) ||
            (funct3 == mosaic_pkg::F3_OR)      || (funct3 == mosaic_pkg::F3_AND)) begin
          legal = 1'b1;
        end
      end

      // -------------------------------------------------------------- jalr
      mosaic_pkg::OP_JALR: begin
        // jalr has exactly one funct3; every other value is reserved.
        if (funct3 == mosaic_pkg::F3_ADD_SUB) begin
          ctl.uses_rs1    = 1'b1;
          ctl.uses_imm    = 1'b1;
          ctl.rs1         = rs1_f;
          ctl.rd          = rd_f;
          ctl.imm         = imm_i;
          ctl.reg_write   = (rd_f != 5'd0);
          ctl.is_jalr     = 1'b1;
          ctl.writes_link = 1'b1;
          legal           = 1'b1;
        end
      end

      // --------------------------------------------------------------- jal
      // funct3 is not part of the encoding: every value is jal.
      mosaic_pkg::OP_JAL: begin
        ctl.uses_imm    = 1'b1;
        ctl.imm         = imm_j;
        ctl.rd          = rd_f;
        ctl.reg_write   = (rd_f != 5'd0);
        ctl.is_jal      = 1'b1;
        ctl.writes_link = 1'b1;
        legal           = 1'b1;
      end

      // ------------------------------------------------------------ system
      mosaic_pkg::OP_SYSTEM: begin
        case (funct3)
          mosaic_pkg::F3_ADD_SUB: begin
            // funct3 000 carries the twelve system instructions. Only the three
            // this core defines are legal, and only in their exact encoding:
            // rd and rs1 are not part of ecall/ebreak/mret, so a nonzero value
            // there is a reserved encoding, not "ecall with a don't-care
            // destination".
            if ((rd_f == 5'd0) && (rs1_f == 5'd0)) begin
              case (insn[31:20])
                12'h000: begin ctl.is_system = 1'b1; ctl.is_ecall  = 1'b1; legal = 1'b1; end
                12'h001: begin ctl.is_system = 1'b1; ctl.is_ebreak = 1'b1; legal = 1'b1; end
                12'h302: begin ctl.is_system = 1'b1; ctl.is_mret   = 1'b1; legal = 1'b1; end
                default: ;   // every other imm12 is reserved
              endcase
            end
          end
          // csrrw, csrrs, csrrc.
          mosaic_pkg::F3_SLL, mosaic_pkg::F3_SLT, mosaic_pkg::F3_SLTU: begin
            ctl.is_system   = 1'b1;
            ctl.csr_imm_form = 1'b0;
            ctl.csr_addr    = insn[31:20];
            ctl.uses_rs1    = 1'b1;
            ctl.rs1         = rs1_f;
            ctl.rd          = rd_f;
            ctl.reg_write   = (rd_f != 5'd0);
            ctl.csr_op      = (funct3 == mosaic_pkg::F3_SLL) ? mosaic_pkg::CSR_RW :
                              (funct3 == mosaic_pkg::F3_SLT) ? mosaic_pkg::CSR_RS : mosaic_pkg::CSR_RC;
            // csrrw always writes; csrrs/csrrc write only when rs1 != x0.
            // csrrw reads only when rd != x0, because with rd == x0 there is
            // nowhere to put the old value -- that read must not happen, since
            // it could have side effects.
            ctl.csr_writes = MutCsrIntent ? 1'b1
                              : (ctl.csr_op == mosaic_pkg::CSR_RW) || (rs1_f != 5'd0);
            ctl.csr_reads  = MutCsrIntent ? 1'b1
                              : (ctl.csr_op != mosaic_pkg::CSR_RW) || (rd_f != 5'd0);
            legal          = 1'b1;
          end
          // csrrwi, csrrsi, csrrci: insn[19:15] is a zero-extended 5-bit
          // immediate, not a register index. funct3 101/110/111, cross-checked
          // against riscv64-elf-objdump -M no-aliases.
          mosaic_pkg::F3_SRL_SRA, mosaic_pkg::F3_OR, mosaic_pkg::F3_AND: begin
            ctl.is_system    = 1'b1;
            ctl.csr_imm_form = 1'b1;
            ctl.csr_addr     = insn[31:20];
            ctl.uses_rs1     = 1'b1;
            ctl.rs1          = rs1_f;
            ctl.rd           = rd_f;
            ctl.reg_write    = (rd_f != 5'd0);
            ctl.csr_op       = (funct3 == mosaic_pkg::F3_SRL_SRA) ? mosaic_pkg::CSR_RW :
                               (funct3 == mosaic_pkg::F3_OR)      ? mosaic_pkg::CSR_RS : mosaic_pkg::CSR_RC;
            ctl.csr_writes = MutCsrIntent ? 1'b1
                              : (ctl.csr_op == mosaic_pkg::CSR_RW) || (rs1_f != 5'd0);
            ctl.csr_reads  = MutCsrIntent ? 1'b1
                              : (ctl.csr_op != mosaic_pkg::CSR_RW) || (rd_f != 5'd0);
            legal           = 1'b1;
          end
          // funct3 100 is reserved: there is no fourth register form and no
          // encoding with neither a register nor an immediate.
          default: ;
        endcase
      end

      // ------------------------------------------------------ OP-32 (W forms)
      // Opcode 0111011 carries the 32-bit-result arithmetic forms, and RV64I is
      // where they live: RV32I defines no OP-32 at all, and the "W" suffix means
      // "word", not "RV32". Two classes:
      //
      //   * funct7 0000000 / 0100000 -> addw, subw, sllw, srlw, sraw, selected
      //     by funct3 exactly as add/sub/sll/srl/sra are selected on OP. They
      //     are R-type: rs1 and rs2 are both registers, the shift amount is
      //     rs2[4:0], and there is no immediate in this encoding (the
      //     shamt-immediate forms are slliw/srliw/sraiw on OP-IMM-32).
      //   * funct7 0000001 -> the M extension's word forms, mulw divw divuw
      //     remw remuw, with funct3 001/010/011 reserved because there is no
      //     mulhw/mulhsuw/mulhuw.
      //
      // Everything else on this opcode is reserved. Decoding the first class as
      // illegal was a defect in this decoder: a core that traps addw breaks any
      // compiler that emits it. riscv64-elf-objdump -d -M no-aliases over the
      // ten encodings is quoted in results/reports/I-010-011-decode-alu.md.
      mosaic_pkg::OP_32: begin
        ctl.uses_rs1  = 1'b1;
        ctl.uses_rs2  = 1'b1;
        ctl.rs1       = rs1_f;
        ctl.rs2       = rs2_f;
        ctl.rd        = rd_f;
        ctl.reg_write = (rd_f != 5'd0);
        if (funct7 == F7_M) begin
          ctl.is_muldiv = 1'b1;
          ctl.md_op     = mosaic_pkg::md_op_e'(funct3);
          ctl.md_signed = md_is_signed;
          ctl.md_w      = 1'b1;
          // The three high-half funct3 values are reserved, not "mulh with a
          // word result": there is no such instruction, and a reserved encoding
          // that half-works is a reserved encoding that becomes a bug.
          legal = (funct3 != 3'b001) && (funct3 != 3'b010) && (funct3 != 3'b011);
        end else if (!MutWIFormsIllegal) begin
          ctl.uses_alu = 1'b1;
          legal        = 1'b1;
          case (funct7)
            F7_BASE: begin
              case (funct3)
                mosaic_pkg::F3_ADD_SUB:  ctl.alu_op = mosaic_pkg::ALU_ADDW;  // addw
                mosaic_pkg::F3_SLL:      ctl.alu_op = mosaic_pkg::ALU_SLLW;  // sllw
                mosaic_pkg::F3_SRL_SRA:  ctl.alu_op = mosaic_pkg::ALU_SRLW;  // srlw
                default:                 legal = 1'b0;
              endcase
            end
            F7_ALT: begin
              case (funct3)
                mosaic_pkg::F3_ADD_SUB:  ctl.alu_op = mosaic_pkg::ALU_SUBW;  // subw
                mosaic_pkg::F3_SRL_SRA:  ctl.alu_op = mosaic_pkg::ALU_SRAW;  // sraw
                default:                 legal = 1'b0;
              endcase
            end
            default: legal = 1'b0;
          endcase
        end
      end

      // --------------------------------------------------- everything else
      // Opcodes not listed above are not RV64IM: the custom-0 and custom-1
      // spaces, the A extension (0100000-0101111), the F/D/Q opcodes, the OP-32
      // float space, the other privileged instructions (1110100-1110111,
      // 1111000-1111011) and everything unassigned. None of them is a
      // compressed instruction either: a 16-bit compressed instruction reaches
      // the decoder with insn[1:0] != 11 and lands on one of these opcodes,
      // which is exactly why I-041 decompresses before here rather than after.
      default: legal = 1'b0;
    endcase

    // An arm that decided halfway through that the encoding is reserved may
    // already have written fields; the illegal result must not keep them.
    if (legal) begin
      ctl.valid   = 1'b1;
      ctl.illegal = 1'b0;
    end else begin
      ctl = CTL_ILLEGAL;
    end
  end

endmodule

`default_nettype wire
