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
// Mutation hooks
// --------------
// The shipping build defines none of the `MOSAIC_DECODER_MUTANT_*` macros. Each
// injects exactly one broken behaviour so the unit test can be shown to detect
// it; the mutant table is in results/reports/I-010-011-decode-alu.md. They
// exist to prove the test has teeth and have no place in any other build.

`default_nettype none

module mosaic_decoder (
    input  wire  [31:0]             insn,
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
`ifdef MOSAIC_DECODER_MUTANT_RV32_WORD_LEGAL
  // The mutation arm is `ifdef'd directly in the case statement below; there
  // is no localparam for it, because the whole arm *is* the mutation.
`else
  // The shipping build decodes no OP-32 arm at all: addw/subw/sllw/srlw/sraw
  // are RV32-only and reserved on RV64.
`endif
`ifdef MOSAIC_DECODER_MUTANT_RESERVED_F3_LEGAL
  localparam bit MutReservedF3 = 1'b1;   // LOAD funct3 111 accepted
`else
  localparam bit MutReservedF3 = 1'b0;
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

  // M-extension signedness: the three unsigned operations are exactly the
  // three funct3 patterns with all three bits set (mulhu, divu, remu).
  logic md_is_signed;
  assign md_is_signed = !(funct3[2] && funct3[1] && funct3[0]);

  // The one and only illegal state. Every field is a defined constant, so an
  // illegal encoding cannot leave a stale rd or a stale mem_size behind, and
  // "the decoder drove nothing" is a single named value rather than a list of
  // defaults that can drift apart.
  localparam decode_ctl_t CTL_ILLEGAL = '{
      valid:        1'b0,
      illegal:      1'b1,
      alu_op:       ALU_PASSB,   // defined "no ALU operation" encoding
      md_op:        MD_MUL,
      mem_kind:     MEM_NONE,
      mem_size:     SZ_BYTE,
      csr_op:       CSR_NONE,
      default:      '0
  };

  always_comb begin : decode
    // `legal` is the single source of truth for valid/illegal: an arm that
    // forgets to set it produces a fully illegal ctl, not a half-decoded one.
    bit legal;

    ctl  = CTL_ILLEGAL;
    legal = 1'b0;

    case (opcode)
      // ------------------------------------------------------------- loads
      OP_LOAD: begin
        ctl.uses_rs1  = 1'b1;
        ctl.uses_imm  = 1'b1;
        ctl.rs1       = rs1_f;
        ctl.rd        = rd_f;
        ctl.imm       = imm_i;
        ctl.reg_write = (rd_f != 5'd0);
        ctl.mem_kind  = MEM_LOAD;
        ctl.uses_alu  = 1'b1;
        ctl.alu_op    = ALU_ADD;   // effective address = rs1 + imm
        legal         = 1'b1;
        case (funct3)
          F3_ADD_SUB: begin ctl.mem_size = SZ_BYTE; ctl.mem_signed = 1'b1; end  // lb
          F3_SLL:     begin ctl.mem_size = SZ_HALF; ctl.mem_signed = 1'b1; end  // lh
          F3_SLT:     begin ctl.mem_size = SZ_WORD; ctl.mem_signed = 1'b1; end  // lw
          F3_SLTU:    begin ctl.mem_size = SZ_DBL;  ctl.mem_signed = 1'b1; end  // ld
          F3_XOR:     begin ctl.mem_size = SZ_BYTE; ctl.mem_signed = 1'b0; end  // lbu
          3'd5:       begin ctl.mem_size = SZ_HALF; ctl.mem_signed = 1'b0; end  // lhu
          // funct3 110 and 111 are reserved: RV64I has no load wider than ld
          // and no store-by-width encoding beyond the six defined above.
          3'd7: begin
            // Mutation: accept a reserved funct3 as a signed doubleword load.
            if (MutReservedF3) begin
              ctl.mem_size   = SZ_DBL;
              ctl.mem_signed = 1'b1;
            end else begin
              legal = 1'b0;
            end
          end
          default: legal = 1'b0;
        endcase
      end

      // -------------------------------------------------- fence / fence.i
      OP_MISC_MEM: begin
        // Only funct3 000 (fence) and 001 (fence.i) are defined. The
        // fm/pred/succ fields of fence are all legal values and are not
        // decoded here: the memory system owns them.
        case (funct3)
          F3_ADD_SUB: begin ctl.is_miscmem = 1'b1; ctl.is_fence_i = 1'b0; end
          F3_SLL:     begin ctl.is_miscmem = 1'b1; ctl.is_fence_i = 1'b1; end
          default:     legal = 1'b0;
        endcase
      end

      // ----------------------------------------------------------- OP-IMM
      OP_IMM: begin
        ctl.uses_rs1  = 1'b1;
        ctl.uses_imm  = 1'b1;
        ctl.rs1       = rs1_f;
        ctl.rd        = rd_f;
        ctl.uses_alu  = 1'b1;
        ctl.reg_write = (rd_f != 5'd0);
        ctl.imm       = imm_i;
        legal         = 1'b1;
        case (funct3)
          F3_ADD_SUB: ctl.alu_op = ALU_ADD;    // addi
          F3_SLL: begin                          // slli
            ctl.alu_op = ALU_SLL;
            // Mutation: ignore insn[31:26], so "shift by 64" (shamt field
            // zero with a nonzero upper field) decodes as a legal slli.
            if (MutShiftUpper || (insn[31:26] == 6'h00)) ctl.imm = imm_slli;
            else                                          legal = 1'b0;
          end
          F3_SLT:  ctl.alu_op = ALU_SLT;        // slti
          F3_SLTU: ctl.alu_op = ALU_SLTU;       // sltiu; immediate sign-extended
          F3_XOR:  ctl.alu_op = ALU_XOR;        // xori
          F3_SRL_SRA: begin                       // srli / srai
            // funct3 101 alone does not say which one: insn[31:26] does.
            // Mutation: drop the funct3 101 requirement from the srai arm.
            if (insn[31:26] == 6'h00) begin
              ctl.alu_op = ALU_SRL;
              ctl.imm    = imm_slli;
            end else if ((insn[31:26] == 6'h10) &&
                         (MutSraiFunct3 || (funct3 == F3_SRL_SRA))) begin
              ctl.alu_op = ALU_SRA;
              ctl.imm    = imm_slli;
            end else begin
              legal = 1'b0;
            end
          end
          F3_OR:   ctl.alu_op = ALU_OR;         // ori
          F3_AND:  ctl.alu_op = ALU_AND;        // andi
          default: legal = 1'b0;
        endcase
      end

      // ------------------------------------------------------------- auipc
      // funct3 is not part of the encoding: every value is auipc.
      OP_AUIPC: begin
        ctl.uses_imm  = 1'b1;
        ctl.imm       = imm_u;
        ctl.rd        = rd_f;
        ctl.reg_write = (rd_f != 5'd0);
        ctl.is_auipc  = 1'b1;      // ALU operand A is the PC
        ctl.uses_alu  = 1'b1;
        ctl.alu_op    = ALU_ADD;
        legal         = 1'b1;
      end

      // ------------------------------------------------------------ stores
      OP_STORE: begin
        ctl.uses_rs1 = 1'b1;
        ctl.uses_rs2 = 1'b1;
        ctl.uses_imm = 1'b1;
        ctl.rs1      = rs1_f;
        ctl.rs2      = rs2_f;     // insn[11:7] is imm[4:0], not rd
        ctl.imm      = imm_s;
        ctl.mem_kind = MEM_STORE;
        ctl.uses_alu = 1'b1;
        ctl.alu_op   = ALU_ADD;
        legal        = 1'b1;
        case (funct3)
          F3_ADD_SUB: ctl.mem_size = SZ_BYTE;   // sb
          3'd1:       ctl.mem_size = SZ_HALF;   // sh
          F3_SLT:     ctl.mem_size = SZ_WORD;   // sw
          3'd3:       ctl.mem_size = SZ_DBL;    // sd
          default:    legal = 1'b0;              // funct3 100..111 reserved
        endcase
      end

      // ------------------------------------------------------- OP-IMM-32
      // RV64 only: the 32-bit word forms live on OP-32, which is illegal here.
      OP_IMM_32: begin
        ctl.uses_rs1  = 1'b1;
        ctl.uses_imm  = 1'b1;
        ctl.rs1       = rs1_f;
        ctl.rd        = rd_f;
        ctl.reg_write = (rd_f != 5'd0);
        ctl.uses_alu  = 1'b1;
        ctl.imm       = imm_i;
        legal         = 1'b1;
        case (funct3)
          F3_ADD_SUB: ctl.alu_op = ALU_ADDW;    // addiw
          F3_SLL: begin                          // slliw
            ctl.alu_op = ALU_SLLW;
            if (funct7 == F7_BASE) ctl.imm = {59'd0, shamt_w};
            else                   legal = 1'b0;
          end
          F3_SRL_SRA: begin                       // srliw / sraiw
            if (funct7 == F7_BASE) begin
              ctl.alu_op = ALU_SRLW;
              ctl.imm    = {59'd0, shamt_w};
            end else if (funct7 == F7_ALT) begin
              ctl.alu_op = ALU_SRAW;
              ctl.imm    = {59'd0, shamt_w};
            end else begin
              legal = 1'b0;
            end
          end
          default: legal = 1'b0;
        endcase
      end

      // ------------------------------------------------------- OP and M
      OP_MUL_DIV: begin
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
          ctl.md_op     = md_op_e'(funct3);
          ctl.md_signed = md_is_signed;
        end else begin
          ctl.uses_alu = 1'b1;
          case (funct3)
            F3_ADD_SUB: begin
              if (funct7 == F7_BASE)     ctl.alu_op = ALU_ADD;
              else if (funct7 == F7_ALT) ctl.alu_op = ALU_SUB;
              else                          legal = 1'b0;
            end
            F3_SLL: begin
              if (funct7 == F7_BASE)     ctl.alu_op = ALU_SLL;
              else                          legal = 1'b0;
            end
            F3_SLT: begin
              if (funct7 == F7_BASE)     ctl.alu_op = ALU_SLT;
              else                          legal = 1'b0;
            end
            F3_SLTU: begin
              if (funct7 == F7_BASE)     ctl.alu_op = ALU_SLTU;
              else                          legal = 1'b0;
            end
            F3_XOR: begin
              if (funct7 == F7_BASE)     ctl.alu_op = ALU_XOR;
              else                          legal = 1'b0;
            end
            F3_SRL_SRA: begin
              if (funct7 == F7_BASE)     ctl.alu_op = ALU_SRL;
              else if (funct7 == F7_ALT) ctl.alu_op = ALU_SRA;
              else                          legal = 1'b0;
            end
            F3_OR: begin
              if (funct7 == F7_BASE)     ctl.alu_op = ALU_OR;
              else                          legal = 1'b0;
            end
            default: begin
              if (funct7 == F7_BASE)     ctl.alu_op = ALU_AND;
              else                          legal = 1'b0;
            end
          endcase
        end
      end

      // ------------------------------------------------------------ branch
      OP_BRANCH: begin
        ctl.uses_rs1     = 1'b1;
        ctl.uses_rs2     = 1'b1;
        ctl.uses_imm     = 1'b1;
        ctl.rs1          = rs1_f;
        ctl.rs2          = rs2_f;   // insn[11:7] is imm[4:1], not rd
        ctl.imm          = imm_b;
        ctl.is_branch    = 1'b1;
        ctl.branch_funct = funct3;
        // beq bne blt bge bltu bgeu; funct3 100 is the only reserved value.
        if ((funct3 == F3_ADD_SUB) || (funct3 == F3_SLL) ||
            (funct3 == F3_SLT)     || (funct3 == F3_SRL_SRA) ||
            (funct3 == F3_SLTU)    || (funct3 == F3_AND)) begin
          legal = 1'b1;
        end
      end

      // -------------------------------------------------------------- jalr
      OP_JALR: begin
        // jalr has exactly one funct3; every other value is reserved.
        if (funct3 == F3_ADD_SUB) begin
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
      OP_JAL: begin
        ctl.uses_imm    = 1'b1;
        ctl.imm         = imm_j;
        ctl.rd          = rd_f;
        ctl.reg_write   = (rd_f != 5'd0);
        ctl.is_jal      = 1'b1;
        ctl.writes_link = 1'b1;
        legal           = 1'b1;
      end

      // ------------------------------------------------------------ system
      OP_SYSTEM: begin
        case (funct3)
          F3_ADD_SUB: begin
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
          F3_SLL, F3_SLT, F3_SLTU: begin
            ctl.is_system   = 1'b1;
            ctl.csr_imm_form = 1'b0;
            ctl.csr_addr    = insn[31:20];
            ctl.uses_rs1    = 1'b1;
            ctl.rs1         = rs1_f;
            ctl.rd          = rd_f;
            ctl.reg_write   = (rd_f != 5'd0);
            ctl.csr_op      = (funct3 == F3_SLL) ? CSR_RW :
                              (funct3 == F3_SLT) ? CSR_RS : CSR_RC;
            // csrrw always writes; csrrs/csrrc write only when rs1 != x0.
            // csrrw reads only when rd != x0, because with rd == x0 there is
            // nowhere to put the old value -- that read must not happen, since
            // it could have side effects.
            ctl.csr_writes = MutCsrIntent ? 1'b1
                              : (ctl.csr_op == CSR_RW) || (rs1_f != 5'd0);
            ctl.csr_reads  = MutCsrIntent ? 1'b1
                              : (ctl.csr_op != CSR_RW) || (rd_f != 5'd0);
            legal          = 1'b1;
          end
          // csrrwi, csrrsi, csrrci: insn[19:15] is a zero-extended 5-bit
          // immediate, not a register index. funct3 101/110/111, cross-checked
          // against riscv64-elf-objdump -M no-aliases.
          F3_SRL_SRA, F3_OR, F3_AND: begin
            ctl.is_system    = 1'b1;
            ctl.csr_imm_form = 1'b1;
            ctl.csr_addr     = insn[31:20];
            ctl.uses_rs1     = 1'b1;
            ctl.rs1          = rs1_f;
            ctl.rd           = rd_f;
            ctl.reg_write    = (rd_f != 5'd0);
            ctl.csr_op       = (funct3 == F3_SRL_SRA) ? CSR_RW :
                               (funct3 == F3_OR)      ? CSR_RS : CSR_RC;
            ctl.csr_writes = MutCsrIntent ? 1'b1
                              : (ctl.csr_op == CSR_RW) || (rs1_f != 5'd0);
            ctl.csr_reads  = MutCsrIntent ? 1'b1
                              : (ctl.csr_op != CSR_RW) || (rd_f != 5'd0);
            legal           = 1'b1;
          end
          // funct3 100 is reserved: there is no fourth register form and no
          // encoding with neither a register nor an immediate.
          default: ;
        endcase
      end

`ifdef MOSAIC_DECODER_MUTANT_RV32_WORD_LEGAL
      // ------------------------------------------------------ OP-32 (RV32)
      // Mutation only. The RV32-only word forms decode as if this were an RV32
      // core; on RV64 they are reserved and must raise an illegal instruction.
      7'b0111011: begin
        ctl.uses_rs1  = 1'b1;
        ctl.uses_rs2  = 1'b1;
        ctl.uses_imm  = 1'b1;
        ctl.rs1       = rs1_f;
        ctl.rs2       = rs2_f;
        ctl.rd        = rd_f;
        ctl.reg_write = (rd_f != 5'd0);
        ctl.imm       = imm_i;
        ctl.uses_alu  = 1'b1;
        legal         = 1'b1;
        case (funct3)
          F3_ADD_SUB: begin
            if (funct7 == F7_BASE)     ctl.alu_op = ALU_ADDW;
            else if (funct7 == F7_ALT) ctl.alu_op = ALU_SUBW;
            else                          legal = 1'b0;
          end
          F3_SLL: begin
            if (funct7 == F7_BASE)     ctl.alu_op = ALU_SLLW;
            else                          legal = 1'b0;
          end
          F3_SRL_SRA: begin
            if (funct7 == F7_BASE)     ctl.alu_op = ALU_SRLW;
            else if (funct7 == F7_ALT) ctl.alu_op = ALU_SRAW;
            else                          legal = 1'b0;
          end
          default: legal = 1'b0;
        endcase
      end
`endif

      // --------------------------------------------------- everything else
      // Opcodes not listed above are not RV64IM: the custom-0 and custom-1
      // spaces, the A extension (0100000-0101111), OP-32 (0111011, the RV32-only
      // word forms), the F/D/Q opcodes, the OP-32 float space, the other
      // privileged instructions (1110100-1110111, 1111000-1111011) and
      // everything unassigned. None of them is a compressed instruction either:
      // a 16-bit compressed instruction reaches the decoder with insn[1:0] !=
      // 11 and lands on one of these opcodes, which is exactly why I-041
      // decompresses before here rather than after.
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
