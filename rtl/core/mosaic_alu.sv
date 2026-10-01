// ============================================================================
// mosaic_alu -- work package I-011.
//
// The integer ALU of the scalar core: one purely combinational function
//     (a, b, op) -> (result, zero)
// with no state, no clock and no reset. Everything a later pipeline stage needs
// to know about this block is on those five ports.
//
// Contract
// --------
//   * `op` is a `mosaic_pkg::alu_op_e` value carried as its 4-bit encoding.
//     All sixteen encodings are decoded; there is no illegal `op`, because the
//     field is exactly as wide as the enum.
//   * `result` is XLEN bits wide and holds the RV64 definition of the operation
//     for every input pair.
//   * `zero` is 1 exactly when `result == 0`, for every op and every input.
//
// The three rules that RV64 punishes a core for getting wrong
// -----------------------------------------------------------
//   1. The `*W` forms sign-extend. `addw`, `subw`, `sllw`, `srlw` and `sraw`
//      operate on a[31:0] and b[31:0] only, and the 32-bit answer is then
//      sign-extended into the XLEN-bit result. They do *not* zero-extend, and
//      they do not let bits 63:32 of the operands reach the result. So
//      `addw(0x7fffffff, 1)` is 0xffffffff80000000, not 0x0000000080000000.
//   2. Shift amounts are masked to the width of the *destination*, not of the
//      operand. `sll`/`srl`/`sra` use b[5:0] on RV64 (so a shift by 64 is a
//      shift by 0); `sllw`/`srlw`/`sraw` use b[4:0], so a shift of 0x80000000
//      or of 32 is a shift by 0. Reading b[6:0] or b[5:0] for the W forms is the
//      classic RV64 bug.
//   3. Comparisons are full-width 0/1, and the signedness is chosen per op:
//      `slt` is signed so -1 < 1 is 1, `sltu` is unsigned so the same pair
//      gives 0, because 0xffff... is the largest unsigned value there is.
//
// Flags
// -----
// No carry, overflow or sticky output exists, and none is planned here. RV64I
// defines `add`/`sub` to wrap modulo 2^XLEN and `addw`/`subw` to wrap modulo
// 2^32, so the carry-out of the 64-bit adder is not architectural state in this
// profile: the only consumer of a carry in RV64I would be a carry-propagating
// shift, and that is absent from the base ISA (M has its own multiply unit and A
// is not in the p0 profile). Adding an output with no reader would be a wire
// with no reader; the questions that do have an architectural answer in RV64I --
// slt, sltu and the zero flag -- are answered inside this block.
//
// Relationship to the rest of the core
// ------------------------------------
//   * The muldiv unit (I-012) consumes `a`, `b` and the M opcode directly; this
//     block does not implement `mul`/`div`/`rem` and must not grow a partial
//     version of them.
//   * Address generation uses ALU_PASSB to carry an operand through untouched,
//     which is why the pass-through exists as a first-class op rather than as a
//     special case of `add`.
//
// Negative controls
// -----------------
// Every `MOSAIC_ALU_MUTANT_<n>` block below injects one deliberate defect that a
// plausible implementation could carry. All of them are off unless the matching
// -D is passed on the Verilator command line, and each one is demonstrated to
// make CASE=alu.boundaries fail, so the case is known to detect the defect it
// claims to cover rather than merely passing on good RTL.
//

`default_nettype none

module mosaic_alu #(
    parameter int XLEN = 64
) (
    input  wire  [XLEN-1:0]  a,
    input  wire  [XLEN-1:0]  b,
    input  wire  [3:0]       op,      // mosaic_pkg::alu_op_e
    output logic [XLEN-1:0]  result,
    output logic             zero     // result == 0
);
  // The enum encoding is the contract with the decoder (I-010) and with the
  // testbenches; nothing here is allowed to invent a second numbering.
  import mosaic_pkg::*;

  // ---------------------------------------------------------------- geometry
  // The W forms are 32 bits in RV64 whatever XLEN is; the rest follow XLEN.
  localparam int WLEN     = 32;
  localparam int SHAMT_W  = $clog2(XLEN);  // 6 on RV64: the 64-bit shift amount
  localparam int WSHAMT_W = 5;            // the W shift amount, always 5 bits

  localparam logic [XLEN-1:0] LOGIC_ONE  = {{(XLEN - 1){1'b0}}, 1'b1};
  localparam logic [XLEN-1:0] LOGIC_ZERO = {XLEN{1'b0}};

  // ------------------------------------------------------------- intermediates
  logic [SHAMT_W-1:0] shamt_wide;  // b[5:0]: shift amount for the XLEN-bit shifts
  logic [WSHAMT_W-1:0] shamt_w;    // b[4:0]: shift amount for the W shifts
  logic [WLEN-1:0] a_low;          // a[31:0], the W operand
  logic [WLEN-1:0] b_low;          // b[31:0], the W operand
  logic [WLEN-1:0] word;           // 32-bit answer of a W form, before extension
  logic [XLEN-1:0] sra_result;     // arithmetic shift right, XLEN bits
  logic lt_signed;                 // ($signed(a) <  $signed(b))
  logic lt_unsigned;               // (         a  <          b )

  // Sign-extend a W answer into the XLEN result lane. This is the single place
  // the RV64 W rule is implemented, so there is exactly one expression that has
  // to be right.
  function automatic logic [XLEN-1:0] extend_word(input logic [WLEN-1:0] value);
`ifdef MOSAIC_ALU_MUTANT_1
    // NEGATIVE CONTROL: zero-extend the W answer instead of sign-extending it.
    extend_word = {{(XLEN - WLEN){1'b0}}, value};
`else
    extend_word = {{(XLEN - WLEN){value[WLEN-1]}}, value};
`endif
  endfunction

  always_comb begin
    // Defaults first, so no intermediate can latch: the ops that leave these
    // untouched are the non-W ops, which never read them.
    shamt_wide   = b[SHAMT_W-1:0];
    shamt_w      = b[WSHAMT_W-1:0];
    a_low        = a[WLEN-1:0];
    b_low        = b[WLEN-1:0];
    word         = {WLEN{1'b0}};
    sra_result   = LOGIC_ZERO;
    lt_signed    = 1'b0;
    lt_unsigned  = 1'b0;

    // `slt` is the signed comparison and `sltu` the unsigned one over the same
    // pair of wires. The operands are cast rather than redeclared, so the two
    // readings cannot silently drift apart.
    lt_signed    = ($signed(a) < $signed(b));
    lt_unsigned  = ($unsigned(a) < $unsigned(b));

`ifdef MOSAIC_ALU_MUTANT_2
    // NEGATIVE CONTROL: sra implemented as a logical shift, so a negative value
    // shifts zeros in instead of replicating its sign bit.
    sra_result   = (a >> shamt_wide);
`else
    sra_result   = ($signed(a) >>> shamt_wide);
`endif

    case (op)
      ALU_ADD:  result = a + b;
      ALU_SUB:  result = a - b;
      ALU_SLL:  result = a << shamt_wide;
      ALU_SRL:  result = a >> shamt_wide;
      ALU_SRA:  result = sra_result;
      ALU_XOR:  result = a ^ b;
      ALU_OR:   result = a | b;
      ALU_AND:  result = a & b;

      ALU_SLT:
`ifdef MOSAIC_ALU_MUTANT_4
        // NEGATIVE CONTROL: slt implemented with the unsigned comparison, so
        // -1 < 1 answers 0.
        result = lt_unsigned ? LOGIC_ONE : LOGIC_ZERO;
`else
        result = lt_signed ? LOGIC_ONE : LOGIC_ZERO;
`endif

      ALU_SLTU: result = lt_unsigned ? LOGIC_ONE : LOGIC_ZERO;

      ALU_ADDW: begin
        word   = a_low + b_low;
        result = extend_word(word);
      end

      ALU_SUBW: begin
        word   = a_low - b_low;
        result = extend_word(word);
      end

      ALU_SLLW: begin
`ifdef MOSAIC_ALU_MUTANT_3
        // NEGATIVE CONTROL: six shift bits on a 32-bit operand, so a shift
        // amount of 32..63 does not wrap back to 0.
        word   = a_low << b[SHAMT_W-1:0];
`else
        word   = a_low << shamt_w;
`endif
        result = extend_word(word);
      end

      ALU_SRLW: begin
        word   = a_low >> shamt_w;
        result = extend_word(word);
      end

      ALU_SRAW: begin
        word   = $signed(a_low) >>> shamt_w;
        result = extend_word(word);
      end

      ALU_PASSB: result = b;

      // Unreachable: every one of the sixteen 4-bit encodings is covered above.
      // Present so that a future edit which adds an op cannot fall through
      // carrying the previous case's value.
      default:   result = LOGIC_ZERO;
    endcase

`ifdef MOSAIC_ALU_MUTANT_5
    // NEGATIVE CONTROL: the zero flag reports on `a` instead of on `result`.
    zero = (a == LOGIC_ZERO);
`else
    // Derived from the answer, never from the inputs: the word forms and the
    // comparisons both produce zero from operands that are not zero.
    zero = (result == LOGIC_ZERO);
`endif
  end

endmodule : mosaic_alu

`default_nettype wire
