// ============================================================================
// mosaic_amo_alu -- work package I-039: the atomic read-modify-write datapath.
//
// Pure combinational. Given the aligned doubleword the memory returned, the
// operand and the operation, it produces the doubleword to write back with the
// accessed field replaced. It knows the two things that decide the result and
// nothing else:
//
//   * the *width* (SZ_WORD / SZ_DBL) -- AMO*.W is a 32-bit operation whose
//     operand is the low 32 bits of rs2; AMO*.D is 64-bit;
//   * the *signedness of the comparison*, which is a property of the operation
//     and not of the access: AMOMIN/AMOMAX compare their operands as signed
//     two's-complement, AMOMINU/AMOMAXU as unsigned. Folding the two together
//     is the signed/unsigned-boundary defect the case names, and it is the
//     reason `amo_op` carries the operation rather than a single "signed" bit.
//
// The field is extracted by its lane, replaced, and shifted back, so a
// neighbouring byte of the doubleword is never disturbed.
// ============================================================================

`ifndef MOSAIC_AMO_ALU_SV_
`define MOSAIC_AMO_ALU_SV_

`default_nettype none
`resetall

/* verilator lint_off UNUSEDPARAM */
`include "mosaic_cfg_pkg.svh"
/* verilator lint_on UNUSEDPARAM */

module mosaic_amo_alu (
    input  mosaic_pkg::amo_op_e op_i,
    input  logic [2:0]          size_i,     // mosaic_pkg::SZ_WORD or SZ_DBL
    input  logic [2:0]          lane_i,     // addr[2:0], the field's byte offset
    input  logic [63:0]         old_win_i,  // the aligned doubleword read back
    input  logic [63:0]         operand_i,  // rs2, at lane 0
    output logic [63:0]         new_win_o   // old_win with the field replaced
);

  // The arithmetic (non-comparison) operations, shared by both widths.
  function automatic logic [31:0] arith_w(input mosaic_pkg::amo_op_e op,
                                          input logic [31:0] a,
                                          input logic [31:0] b);
    begin
      case (op)
        mosaic_pkg::AMO_ADD:  arith_w = a + b;
        mosaic_pkg::AMO_SWAP: arith_w = b;
        mosaic_pkg::AMO_XOR:  arith_w = a ^ b;
        mosaic_pkg::AMO_AND:  arith_w = a & b;
        mosaic_pkg::AMO_OR:   arith_w = a | b;
        default:              arith_w = a;
      endcase
    end
  endfunction

  function automatic logic [63:0] arith_d(input mosaic_pkg::amo_op_e op,
                                          input logic [63:0] a,
                                          input logic [63:0] b);
    begin
      case (op)
        mosaic_pkg::AMO_ADD:  arith_d = a + b;
        mosaic_pkg::AMO_SWAP: arith_d = b;
        mosaic_pkg::AMO_XOR:  arith_d = a ^ b;
        mosaic_pkg::AMO_AND:  arith_d = a & b;
        mosaic_pkg::AMO_OR:   arith_d = a | b;
        default:              arith_d = a;
      endcase
    end
  endfunction

`ifdef MOSAIC_AMO_MUTANT_MINMAX_SIGNED
  // NEGATIVE CONTROL: the signed and unsigned comparison operations are
  // exchanged, so AMOMIN/AMOMAX compare as unsigned and AMOMINU/AMOMAXU as
  // signed. The defect is invisible until an operand's top bit is set -- which
  // is exactly the boundary CASE=amo.linearization drives.
  function automatic logic [31:0] amo_w(input mosaic_pkg::amo_op_e op,
                                        input logic [31:0] a,
                                        input logic [31:0] b);
    begin
      case (op)
        mosaic_pkg::AMO_MIN:  amo_w = (a < b) ? a : b;
        mosaic_pkg::AMO_MAX:  amo_w = (a > b) ? a : b;
        mosaic_pkg::AMO_MINU: amo_w = ($signed(a) < $signed(b)) ? a : b;
        mosaic_pkg::AMO_MAXU: amo_w = ($signed(a) > $signed(b)) ? a : b;
        default:              amo_w = arith_w(op, a, b);
      endcase
    end
  endfunction

  function automatic logic [63:0] amo_d(input mosaic_pkg::amo_op_e op,
                                        input logic [63:0] a,
                                        input logic [63:0] b);
    begin
      case (op)
        mosaic_pkg::AMO_MIN:  amo_d = (a < b) ? a : b;
        mosaic_pkg::AMO_MAX:  amo_d = (a > b) ? a : b;
        mosaic_pkg::AMO_MINU: amo_d = ($signed(a) < $signed(b)) ? a : b;
        mosaic_pkg::AMO_MAXU: amo_d = ($signed(a) > $signed(b)) ? a : b;
        default:              amo_d = arith_d(op, a, b);
      endcase
    end
  endfunction
`else
  function automatic logic [31:0] amo_w(input mosaic_pkg::amo_op_e op,
                                        input logic [31:0] a,
                                        input logic [31:0] b);
    begin
      case (op)
        mosaic_pkg::AMO_MIN:  amo_w = ($signed(a) < $signed(b)) ? a : b;
        mosaic_pkg::AMO_MAX:  amo_w = ($signed(a) > $signed(b)) ? a : b;
        mosaic_pkg::AMO_MINU: amo_w = (a < b) ? a : b;
        mosaic_pkg::AMO_MAXU: amo_w = (a > b) ? a : b;
        default:              amo_w = arith_w(op, a, b);
      endcase
    end
  endfunction

  function automatic logic [63:0] amo_d(input mosaic_pkg::amo_op_e op,
                                        input logic [63:0] a,
                                        input logic [63:0] b);
    begin
      case (op)
        mosaic_pkg::AMO_MIN:  amo_d = ($signed(a) < $signed(b)) ? a : b;
        mosaic_pkg::AMO_MAX:  amo_d = ($signed(a) > $signed(b)) ? a : b;
        mosaic_pkg::AMO_MINU: amo_d = (a < b) ? a : b;
        mosaic_pkg::AMO_MAXU: amo_d = (a > b) ? a : b;
        default:              amo_d = arith_d(op, a, b);
      endcase
    end
  endfunction
`endif

  logic [63:0] old_field_c;
  logic [63:0] new_field_c;
  logic [63:0] mask_c;

  assign old_field_c = old_win_i >> {lane_i, 3'b000};

  always_comb begin
    if (size_i == mosaic_pkg::SZ_DBL) begin
      new_field_c = amo_d(op_i, old_field_c, operand_i);
      mask_c      = 64'hFFFF_FFFF_FFFF_FFFF;
    end else begin
      new_field_c = {32'd0, amo_w(op_i, old_field_c[31:0], operand_i[31:0])};
      mask_c      = 64'h0000_0000_FFFF_FFFF;
    end
  end

  assign new_win_o = (old_win_i & ~(mask_c << {lane_i, 3'b000})) |
                     ((new_field_c & mask_c) << {lane_i, 3'b000});

endmodule

`endif  // MOSAIC_AMO_ALU_SV_
