// ============================================================================
// mosaic_muldiv -- work package I-012: the shared iterative MUL/DIV unit.
//
// One operation in flight, a shift-add multiplier and a restoring divider
// sharing one iteration counter, and a result that is held until the consumer
// takes it. No result FIFO: the card's documented failure is a long-latency unit
// that drops a result when its FIFO is full, and a unit that has no FIFO cannot
// have that failure. What it has instead is one output register set that stays
// put until `res_ready_i`, and a counter (`o_busy`) that says an operation is
// still owned by this unit.
//
// ------------------------------------------------------------------ latency
//
// The iteration count is a function of `req_w_i` alone -- 64 steps for a 64-bit
// operation, 32 for a W form -- and of nothing else. Every step is taken for
// every operand: the multiplier adds `0` when the multiplier bit is clear, and
// the divider compares and subtracts `0` when the divisor is zero. There is no
// early-out on a zero operand, a power-of-two divisor, or a zero dividend,
// because "equal latency for equal control, independent of operand values" is
// a property the RVA23 Zkt work will need and a value-dependent exit path
// would have to be removed to get back.
//
// That also makes the unit a fixed-latency unit whose latency depends only on a
// control bit, which is what lets the testbench cancel at a chosen iteration
// using `o_iter` instead of guessing a cycle count. `o_iter` is the number of
// iterations the operation in flight has completed -- 0 in IDLE, `width` once
// the result has been computed and is waiting to be taken.
//
// The documented latency from the accepting edge to `res_valid_o` is
// `width + 1` cycles: one per iteration, plus the cycle in which the completed
// result is presented. It is the same for every operation at a width and for
// every operand value at a width.
//
// -------------------------------------------------------- the width rule
//
// A W form is the *same operation at 32-bit width*, with the 32-bit answer
// sign-extended into the 64-bit result lane -- not a 64-bit operation whose
// answer happens to be truncated. The distinction is real: for `divuw`, the
// operands are the 32-bit unsigned values, so zero-extending them and dividing
// at 64 bits is wrong (`divuw 0xffffffff, 2` is 0x7fffffff, not 0xffffffff).
//
// So an accepted request carries a *working width*: 32 for a W form, 64
// otherwise. The operands are stored as the raw working-width bit patterns
// zero-extended into a 64-bit register, the sign bits are read at bit
// `width - 1`, the datapath runs `width` iterations, and the final step either
// sign-extends a 32-bit answer or leaves a 64-bit one alone.
//
// The ISA defines five word forms: mulw, divw, divuw, remw, remuw. The
// `req_op_i` port is the full three-bit M-extension encoding, so three more
// combinations exist on the pins that no instruction can reach (the decoder
// leaves the high-half word multiplies reserved). They are still *defined*
// here, by the same rule as everything else -- the high-half multiply at the
// working width -- rather than left to read as don't-care, because an
// undecoded input that produces a defined answer is a unit a later package can
// reason about, and one that produces do-not-care is not.
//
// --------------------------------------------------------------- handshake
//
//   accept    `req_valid_i && req_ready_o`, and `req_ready_o` is high only in
//             IDLE with no flush in that cycle. One operation is accepted and
//             the unit leaves IDLE on the next edge.
//   hold      the result appears with `res_valid_o` high and is *unchanged*
//             until `res_ready_i`. The identity payload (`res_rob_index_o`,
//             `res_rob_gen_o`, `res_uop_index_o`) is latched with the request
//             and travels with the result, so the consumer matches a late
//             result by (rob_index, rob_gen, uop_index) rather than by
//             assuming it is the operation it is still waiting for.
//   flush     `flush_i` has absolute priority. It cancels whatever is in
//             flight, in one cycle, and the unit is ready to accept again on
//             the next cycle. An operation cancelled while it was still
//             iterating and an operation whose *result had already been
//             computed* but not yet accepted are both cancellations, but they
//             are not the same event: `o_cancelled_ctr` counts both and
//             `o_killed_res_ctr` counts the second. A result that was about to
//             be thrown away is the one that says the consumer was late;
//             lumping it in with a partial operation hides that.
//
// `res_valid_o` is masked by the flush, so a result is never offered in a
// flush cycle either. A consumer that loses its own squashed context cannot be
// relied on to ignore a valid result in the flushes' own cycle, and a unit
// that asserts one is asking it to.
//
// ---------------------------------------------------------------- counters
//
// Four counters, all free-running and wrapping, and one invariant the
// testbench checks on every cycle:
//
//     accepted == completed + cancelled + in-flight
//
// where `in-flight` is `o_busy` (1 while an accepted operation has not yet been
// delivered or cancelled). `completed` counts results the consumer took, not
// results that were computed: a computed result that the flush destroys is a
// cancellation, and is counted as one.
//
// --------------------------------------------------------------- what is reset
//
// Reset clears the control state only: the state register, the iteration
// counter and the four counters. The datapath registers are **not** reset.
// Every one of them is written in full when a request is accepted, before any
// step reads it, so no result can ever depend on a stale or undefined value --
// and resetting 128 bits of accumulator per flag is reset cost that buys
// nothing. This is the rule rtl/common/mosaic_ram.sv states: reset cost is
// control state, not DEPTH x WIDTH of storage.
//
// Mutation hooks
// --------------
// The shipping build defines none of the `MOSAIC_MULDIV_MUTANT_*` macros. Each
// injects exactly one broken behaviour so the unit test can be shown to detect
// it; the mutant table is in results/reports/I-012-muldiv.md. They exist to
// prove the test has teeth and have no place in any other build.
// ============================================================================

`default_nettype none
`resetall

/* verilator lint_off UNUSEDPARAM */
/* verilator lint_off UNUSEDSIGNAL */
// `mosaic_pkg` is included rather than imported-and-assumed-present, for the
// reason rtl/core/mosaic_fetch.sv states: Verilator does not search the include
// path for a package on its own, so a file that only names `mosaic_pkg::` is
// resolved against whatever the command line happened to list first. The
// package carries its own include guard, so including it here (and again in the
// testbench wrapper) is idempotent.
`include "mosaic_pkg.sv"
// The generated identity package declares one width per identity field for the
// whole project. This unit names three of them; the rest belong to other
// structures and are unused *here* by construction, not by omission.
//
// UNUSEDSIGNAL is off for the include as well: the package's own `macro_id_live`
// helper does not compare the uop_index field of its two arguments, so three
// bits of each argument are unused *inside the generated file*. That is a
// property of the generated package rather than of this unit, and since
// tools/gen_manifest.py is not this card's to edit, the suppression is scoped to
// the include and reported rather than worked around. See
// results/reports/I-012-muldiv.md.
`include "mosaic_id_pkg.svh"
/* verilator lint_on UNUSEDSIGNAL */
/* verilator lint_on UNUSEDPARAM */

// Widths are declared at file scope because a module's port list cannot see
// declarations inside its own body. They are derived from the generated
// package and nothing else.
localparam int unsigned MD_ROB_INDEX_W = mosaic_id_pkg::MOSAIC_ID_W_ROB_INDEX;
localparam int unsigned MD_ROB_GEN_W   = mosaic_id_pkg::MOSAIC_ID_W_ROB_GEN;
localparam int unsigned MD_UOP_INDEX_W = mosaic_id_pkg::MOSAIC_ID_W_UOP_INDEX;

module mosaic_muldiv (
    input  logic                                clk_i,
    input  logic                                rst_i,

    input  logic                                req_valid_i,
    output logic                                req_ready_o,
    input  mosaic_pkg::md_op_e                  req_op_i,
    input  logic                                req_w_i,
    input  logic [63:0]                         req_a_i,
    input  logic [63:0]                         req_b_i,
    input  logic [MD_ROB_INDEX_W-1:0]           req_rob_index_i,
    input  logic [MD_ROB_GEN_W-1:0]             req_rob_gen_i,
    input  logic [MD_UOP_INDEX_W-1:0]           req_uop_index_i,

    input  logic                                flush_i,

    output logic                                res_valid_o,
    input  logic                                res_ready_i,
    output logic [63:0]                         res_data_o,
    output logic [MD_ROB_INDEX_W-1:0]           res_rob_index_o,
    output logic [MD_ROB_GEN_W-1:0]             res_rob_gen_o,
    output logic [MD_UOP_INDEX_W-1:0]           res_uop_index_o,

    output logic                                o_busy,
    output logic [7:0]                          o_iter,
    output logic [31:0]                         o_accepted_ctr,
    output logic [31:0]                         o_completed_ctr,
    output logic [31:0]                         o_cancelled_ctr,
    output logic [31:0]                         o_killed_res_ctr
);

  // ---------------------------------------------------------------- mutants
`ifdef MOSAIC_MULDIV_MUTANT_FLUSH_IGNORED
  localparam bit MutFlushIgnore = 1'b1;   // flush_i has no effect at all
`else
  localparam bit MutFlushIgnore = 1'b0;
`endif
`ifdef MOSAIC_MULDIV_MUTANT_DROP_UNREADY
  localparam bit MutDropUnready = 1'b1;   // a result leaves DONE without res_ready_i
`else
  localparam bit MutDropUnready = 1'b0;
`endif
`ifdef MOSAIC_MULDIV_MUTANT_RES_STUCK
  localparam bit MutResStuck    = 1'b1;   // the delivered result is never released
`else
  localparam bit MutResStuck    = 1'b0;
`endif
`ifdef MOSAIC_MULDIV_MUTANT_DIV0_ZERO
  localparam bit MutDiv0Zero    = 1'b1;   // divide by zero answers 0, not all ones
`else
  localparam bit MutDiv0Zero    = 1'b0;
`endif
`ifdef MOSAIC_MULDIV_MUTANT_IGNORE_W
  localparam bit MutIgnoreW     = 1'b1;   // a W form runs as a 64-bit operation
`else
  localparam bit MutIgnoreW     = 1'b0;
`endif

  // ----------------------------------------------------------------- state
  typedef enum logic [1:0] {
    ST_IDLE = 2'd0,
    ST_MUL  = 2'd1,
    ST_DIV  = 2'd2,
    ST_DONE = 2'd3
  } state_e;

  state_e      state_r;

  mosaic_pkg::md_op_e op_r;
  logic        w_r;              // working width: 1 = 32-bit W form
  logic [6:0]  width_r;          // 64 or 32; control, never operand data
  logic [63:0] a_r, b_r;         // raw working-width operands, zero-extended
  logic        sign_a_r, sign_b_r;   // operand sign bits, masked by signedness
  logic [6:0]  iter_r;           // iterations completed: 0 .. width_r

  logic [MD_ROB_INDEX_W-1:0] id_rob_index_r;
  logic [MD_ROB_GEN_W-1:0]   id_rob_gen_r;
  logic [MD_UOP_INDEX_W-1:0] id_uop_index_r;

  // multiplier: acc accumulates the product, mcand walks left, mplier right
  logic [127:0] acc_r, mcand_r;
  logic [63:0]  mplier_r;

  // divider: the stored remainder always fits in the working width (it is
  // either below the divisor or the result of subtracting it), but the *shifted*
  // remainder needs one more bit, so `rem_next` is 65 bits and only its low 64
  // are stored.
  logic [63:0]  rem_r;
  logic [63:0]  dsh_r;           // dividend, left-aligned into bit 63
  logic [63:0]  divisor_r;
  logic [63:0]  quot_r;
  logic         q_neg_r, r_neg_r, div_zero_r;

  // ------------------------------------------------------------ helpers
  // Two's-complement negation at the working width. Negating a W form's
  // magnitude in 64 bits would set the upper half, so every negation is masked
  // back to the width it belongs to.
  function automatic logic [63:0] mask_w(input logic [63:0] value,
                                         input logic        word);
    mask_w = word ? {32'd0, value[31:0]} : value;
  endfunction

  function automatic logic [63:0] sext32(input logic [31:0] value);
    sext32 = {{32{value[31]}}, value};
  endfunction

  // ------------------------------------------------------- flush arbitration
  // A flush is honoured in every state, and it is the highest-priority event in
  // the unit: it wins over an iteration step, over a result handshake, and over
  // a request. `req_ready_o` is therefore gated by it, so a request that
  // arrives in a flush cycle is refused rather than accepted and cancelled --
  // the requester is being squashed too, and taking its request would make the
  // unit spend a cycle on an operation nobody will ever consume.
  logic flush_cancel;
  assign flush_cancel = flush_i && !MutFlushIgnore;

  logic req_fire, res_fire, cancel_fire, kill_fire;
  assign req_fire    = req_valid_i && req_ready_o;
  assign res_fire    = res_valid_o && res_ready_i;
  assign cancel_fire = flush_cancel && ((state_r == ST_MUL) || (state_r == ST_DIV));
  assign kill_fire   = flush_cancel && (state_r == ST_DONE);

  assign req_ready_o = (state_r == ST_IDLE) && !flush_cancel;
  assign res_valid_o = (state_r == ST_DONE) && !flush_cancel;
  assign o_busy      = (state_r != ST_IDLE);
  assign o_iter      = {1'b0, iter_r};

  assign res_rob_index_o = id_rob_index_r;
  assign res_rob_gen_o   = id_rob_gen_r;
  assign res_uop_index_o = id_uop_index_r;

  // ------------------------------------------------- acceptance arithmetic
  // W forms take the low 32 bits of the operand registers; every other form
  // takes all 64. `req_a_i[63:32]` is therefore never read for a W form, which
  // is what the interface promises.
  logic        acc_w;
  logic        acc_is_mul;
  logic        acc_sig_a, acc_sig_b;     // signedness of each operand
  logic        acc_a_sign, acc_b_sign;
  logic [63:0] acc_a_eff, acc_b_eff;     // raw working-width operands
  logic [63:0] acc_a_mag, acc_b_mag;     // magnitudes, for the divider
  logic [63:0] acc_dsh;                  // dividend, left-aligned for the loop
  logic        acc_q_neg, acc_r_neg, acc_div_zero;
  logic [127:0] acc_mcand;

  assign acc_w = MutIgnoreW ? 1'b0 : req_w_i;

  // Signedness follows the operation, not the sign of the data: mulhu is
  // unsigned for both operands and mulhsu is signed for one of them, and the
  // mixed case is the whole reason MD_MULHSU exists as its own encoding.
  always_comb begin : op_signedness
    unique case (req_op_i)
      mosaic_pkg::MD_MUL,
      mosaic_pkg::MD_MULH,
      mosaic_pkg::MD_DIV,
      mosaic_pkg::MD_REM:    begin acc_sig_a = 1'b1; acc_sig_b = 1'b1; end
      mosaic_pkg::MD_MULHSU: begin acc_sig_a = 1'b1; acc_sig_b = 1'b0; end
      default:               begin acc_sig_a = 1'b0; acc_sig_b = 1'b0; end
    endcase
  end

  assign acc_is_mul = (req_op_i == mosaic_pkg::MD_MUL)    ||
                      (req_op_i == mosaic_pkg::MD_MULH)   ||
                      (req_op_i == mosaic_pkg::MD_MULHSU) ||
                      (req_op_i == mosaic_pkg::MD_MULHU);

  assign acc_a_eff  = acc_w ? {32'd0, req_a_i[31:0]} : req_a_i;
  assign acc_b_eff  = acc_w ? {32'd0, req_b_i[31:0]} : req_b_i;
  assign acc_a_sign = acc_w ? req_a_i[31] : req_a_i[63];
  assign acc_b_sign = acc_w ? req_b_i[31] : req_b_i[63];
  assign acc_a_mag  = (acc_sig_a && acc_a_sign)
                      ? mask_w(~acc_a_eff + 64'd1, acc_w) : acc_a_eff;
  assign acc_b_mag  = (acc_sig_b && acc_b_sign)
                      ? mask_w(~acc_b_eff + 64'd1, acc_w) : acc_b_eff;

  // Quotient sign is the exclusive-or of the operand signs; the remainder takes
  // the dividend's sign. Both are zero for the unsigned operations.
  assign acc_q_neg = acc_sig_a && (acc_a_sign ^ acc_b_sign);
  assign acc_r_neg = acc_sig_a && acc_a_sign;

  // Dividing by zero is detected once, at acceptance. The restoring loop would
  // already produce an all-ones quotient for it (nothing ever subtracts), but
  // the sign fix-up would then negate it for a negative dividend, so the answer
  // is forced here instead of being left to a coincidence of the algorithm.
  assign acc_div_zero = (acc_b_mag == 64'd0);

  assign acc_dsh   = acc_w ? {acc_a_mag[31:0], 32'd0} : acc_a_mag;
  assign acc_mcand = {64'd0, acc_a_eff};

  // ------------------------------------------------------ result arithmetic
  // Every register these expressions read stops changing when the unit reaches
  // ST_DONE, so the offered result and identity are constant while the unit
  // waits for `res_ready_i`.
  //
  // The unsigned product is the shift-add accumulator. The *signed* product of
  // two working-width operands is that product corrected by the sign bits:
  //     S = P - a_sign * (b << width) - b_sign * (a << width)   (mod 2^128)
  // because a_signed = a_unsigned - a_sign * 2^width, and the 2^(2*width) term
  // falls off the 128-bit end. The low `width` bits are unaffected either way,
  // which is why MUL reads the accumulator and the high-half forms read the
  // corrected product.
  /* verilator lint_off UNUSEDSIGNAL */
  // The corrected product's low bits are the accumulator's, unmodified, and are
  // never read: MUL takes its answer from `acc_r`, and the high-half forms read
  // from bit 32 up. The correction is applied to the whole value because that is
  // what the identity S = P - a_sign*(b<<w) - b_sign*(a<<w) means.
  logic [127:0] prod_shift_a, prod_shift_b, prod_signed;
  /* verilator lint_on UNUSEDSIGNAL */
  assign prod_shift_b = sign_a_r ? ({64'd0, b_r} << width_r) : 128'd0;
  assign prod_shift_a = sign_b_r ? ({64'd0, a_r} << width_r) : 128'd0;
  assign prod_signed  = acc_r - prod_shift_b - prod_shift_a;

  logic [63:0] mul_w;
  always_comb begin : mul_select
    unique case (op_r)
      mosaic_pkg::MD_MULH,
      mosaic_pkg::MD_MULHSU: mul_w = w_r ? {32'd0, prod_signed[63:32]}
                                         : prod_signed[127:64];
      mosaic_pkg::MD_MULHU:  mul_w = w_r ? {32'd0, acc_r[63:32]}
                                         : acc_r[127:64];
      default:               mul_w = w_r ? {32'd0, acc_r[31:0]} : acc_r[63:0];
    endcase
  end

  logic [63:0] quot_mag, rem_mag, quot_sgn, rem_sgn, quot_w, rem_w, div_w;
  assign quot_mag = w_r ? {32'd0, quot_r[31:0]} : quot_r;
  assign rem_mag  = w_r ? {32'd0, rem_r[31:0]}  : rem_r;
  assign quot_sgn = mask_w(q_neg_r ? (~quot_mag + 64'd1) : quot_mag, w_r);
  assign rem_sgn  = mask_w(r_neg_r ? (~rem_mag + 64'd1)  : rem_mag,  w_r);

  // Divide by zero: quotient all ones, remainder the dividend. The dividend is
  // already in `rem_mag` (the loop shifted it through without ever
  // subtracting), so only the quotient needs the explicit answer. Only the
  // *latched* flag is consulted: reading the live request's divisor here would
  // make the offered result depend on inputs the requester may reload while the
  // unit waits, which is precisely the "result changes while waiting" defect.
  assign quot_w = div_zero_r
                  ? (MutDiv0Zero ? 64'd0
                                 : (w_r ? 64'h0000_0000_FFFF_FFFF
                                        : 64'hFFFF_FFFF_FFFF_FFFF))
                  : quot_sgn;
  assign rem_w  = rem_sgn;

  assign div_w = ((op_r == mosaic_pkg::MD_REM) || (op_r == mosaic_pkg::MD_REMU))
                 ? rem_w : quot_w;

  logic is_mul;
  assign is_mul = (op_r == mosaic_pkg::MD_MUL)    ||
                  (op_r == mosaic_pkg::MD_MULH)   ||
                  (op_r == mosaic_pkg::MD_MULHSU) ||
                  (op_r == mosaic_pkg::MD_MULHU);

  logic [63:0] res_calc;
  assign res_calc = mask_w(is_mul ? mul_w : div_w, w_r);

  // A W form's answer is a 32-bit value sign-extended into the 64-bit lane.
  // `res_calc` holds the working-width answer, so the extension is applied
  // exactly once and only for W.
`ifdef MOSAIC_MULDIV_MUTANT_UNSTABLE_RESULT
  // Mutation: the offered result is transparent to the request inputs, so it
  // changes while the unit waits for res_ready_i and while the requester is
  // free to reload them.
  assign res_data_o = (w_r ? sext32(res_calc[31:0]) : res_calc) ^ req_a_i;
`else
  assign res_data_o = w_r ? sext32(res_calc[31:0]) : res_calc;
`endif

  // ------------------------------------------------------------- datapath
  // One step per cycle. `rem_next` is the shifted remainder with the next
  // dividend bit shifted in, and `rem_sub` is that value minus the divisor; a
  // combinational signal each, so the compare, the subtract and the stored
  // remainder below all read the same value rather than three copies of it.
  logic [64:0] rem_next;
  // The subtraction only stores when `rem_next >= divisor`, and in that case
  // the difference is below the divisor and fits in 64 bits (for divisor zero
  // the "difference" is the accumulated dividend, which is also inside 64 bits
  // by the last iteration), so the borrow out of bit 64 is never needed and
  // `rem_sub` is 64 bits wide.
  logic [63:0] rem_sub;
  assign rem_next = {rem_r[63:0], dsh_r[63]};
  assign rem_sub  = rem_next[63:0] - divisor_r;

  // ----------------------------------------------------------- the machine
  always_ff @(posedge clk_i) begin : md_state
    if (rst_i) begin
      state_r <= ST_IDLE;
      iter_r  <= 7'd0;
      o_accepted_ctr   <= 32'd0;
      o_completed_ctr  <= 32'd0;
      o_cancelled_ctr  <= 32'd0;
      o_killed_res_ctr <= 32'd0;
    end else begin
      // Counter conservation: every accepted operation ends in exactly one of
      // completed / cancelled, and `o_busy` is the remainder.
      if (req_fire) begin
        o_accepted_ctr <= o_accepted_ctr + 32'd1;
      end
      if (res_fire) begin
        o_completed_ctr <= o_completed_ctr + 32'd1;
      end
      if (cancel_fire || kill_fire) begin
        o_cancelled_ctr <= o_cancelled_ctr + 32'd1;
      end
      if (kill_fire) begin
        o_killed_res_ctr <= o_killed_res_ctr + 32'd1;
      end

      if (flush_cancel) begin
        // Absolute priority: cancel and be ready again next cycle. The
        // datapath registers are left alone; nothing reads them until the next
        // acceptance writes them in full.
        state_r <= ST_IDLE;
        iter_r  <= 7'd0;
      end else begin
        case (state_r)
          // ------------------------------------------------------- accept
          ST_IDLE: begin
            if (req_valid_i) begin
              op_r      <= req_op_i;
              w_r       <= acc_w;
              width_r   <= acc_w ? 7'd32 : 7'd64;
              a_r       <= acc_a_eff;
              b_r       <= acc_b_eff;
              sign_a_r  <= acc_sig_a && acc_a_sign;
              sign_b_r  <= acc_sig_b && acc_b_sign;
              id_rob_index_r <= req_rob_index_i;
              id_rob_gen_r   <= req_rob_gen_i;
              id_uop_index_r <= req_uop_index_i;
              acc_r     <= 128'd0;
              mcand_r   <= acc_mcand;
              mplier_r  <= acc_b_eff;
              rem_r     <= 64'd0;
              dsh_r     <= acc_dsh;
              divisor_r <= acc_b_mag;
              quot_r    <= 64'd0;
              q_neg_r     <= acc_q_neg;
              r_neg_r     <= acc_r_neg;
              div_zero_r  <= acc_div_zero;
              iter_r    <= 7'd0;
              state_r   <= acc_is_mul ? ST_MUL : ST_DIV;
            end
          end

          // ---------------------------------------------------- multiply
          // Shift-add, one multiplier bit per cycle, most significant bit last.
          // The iteration count is `width_r`, not a value-dependent count.
          ST_MUL: begin
            if (mplier_r[0]) begin
              acc_r <= acc_r + mcand_r;
            end
            mcand_r  <= {mcand_r[126:0], 1'b0};
            mplier_r <= {1'b0, mplier_r[63:1]};
            iter_r   <= iter_r + 7'd1;
            if (iter_r == (width_r - 6'd1)) begin
              state_r <= ST_DONE;
            end
          end

          // ------------------------------------------------------ divide
          // Restoring division, one dividend bit per cycle. The dividend is
          // pre-shifted so the loop always takes bit 63 first and needs no
          // variable index; for a W form the 32 dividend bits start at bits
          // 63:32 and are consumed first.
          ST_DIV: begin
            if (rem_next >= {1'b0, divisor_r}) begin
              rem_r  <= rem_sub;
              quot_r <= {quot_r[62:0], 1'b1};
            end else begin
              rem_r  <= rem_next[63:0];
              quot_r <= {quot_r[62:0], 1'b0};
            end
            dsh_r  <= {dsh_r[62:0], 1'b0};
            iter_r <= iter_r + 7'd1;
            if (iter_r == (width_r - 6'd1)) begin
              state_r <= ST_DONE;
            end
          end

          // --------------------------------------------------------- hold
          // `o_iter` returns to 0 when the operation leaves: "current
          // iteration index" means the index of the operation in flight, and
          // there is none once the result has been taken.
          ST_DONE: begin
            if (res_ready_i || MutDropUnready) begin
              if (!MutResStuck) begin
                state_r <= ST_IDLE;
                iter_r  <= 7'd0;
              end
            end
          end

          default: state_r <= ST_IDLE;
        endcase
      end
    end
  end

endmodule

`resetall
`default_nettype wire
