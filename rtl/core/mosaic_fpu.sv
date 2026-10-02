// ============================================================================
// mosaic_fpu -- work package I-049: the F/D floating-point datapath.
//
// ------------------------------------------------------------ the decision
//
// Selection, recorded here before the code and argued in
// results/reports/I-049-fpu.md: the F/D implementation is **self-implemented in
// SystemVerilog**, not a vendored third-party FPU.
//
// Reasons, in the order they decided it:
//
//   * Licence. This workstation has no network access, so no candidate FPU
//     could be fetched and vendored, and the project may not take material
//     whose licence has not been reviewed. Berkeley SoftFloat (the reference
//     C++ implementation behind most RISC-V FPUs) is BSD-3-Clause and would be
//     *reviewable*, but it is C++ software: it is an oracle, not a datapath,
//     and the card forbids using software as a substitute for a hardware
//     datapath. A vendored RTL FPU (for example a copyleft-licensed academic
//     core) could not be audited for provenance here. Having no candidate whose
//     licence could be reviewed and whose semantics could be checked against
//     the specification, the honest answer is to write the datapath here.
//   * Semantics. This unit's rounding, NaN and flag behaviour is the RISC-V
//     contract, and it is stated below as a policy the testbench checks against
//     an independent oracle. A vendored core would have had to be either
//     accepted as-is (a semantic import this project has not reviewed) or
//     modified, at which point it is ours anyway.
//   * Scope. Writing it here makes the boundary explicit: the operations below
//     are implemented and tested; the ones not written are declared absent in
//     the header, in the report and in the capability matrix. Nothing is faked
//     and nothing falls back to software -- this module has no memory interface,
//     no instruction store and no software-visible state at all, so there is no
//     software path to fall back *to*. That is the whole verification of that
//     claim: a fallback would need somewhere to live, and there is nowhere.
//
// What that costs is stated plainly: `fsqrt.s/d` is **not implemented**, and
// `fdiv.s/d` -- which the card accepts as an alternative to `fsqrt` -- is. The
// capability matrix gives every operation either an implementation or a
// declared-absent line. The machine's advertised ISA still excludes F/D: this
// package delivers the *unit*, and the decode/state/commit integration is
// I-050/V-050's, so nothing in the advertised capability set changed.
//
// ------------------------------------------------------- the operation set
//
// Implemented behind the tagged handshake below:
//
//   fadd.s/d, fsub.s/d, fmul.s/d, fdiv.s/d
//   fsgnj.s/d, fsgnjn.s/d, fsgnjx.s/d
//   fmin.s/d, fmax.s/d
//   feq.s/d, flt.s/d, fle.s/d
//   fclass.s/d
//   fmv.x.w, fmv.w.x, fmv.x.d, fmv.d.x
//   fcvt.w.s, fcvt.wu.s, fcvt.l.s, fcvt.lu.s (and the .d forms)
//   fcvt.s.w, fcvt.s.wu, fcvt.s.l, fcvt.s.lu (and the .d forms)
//   fcvt.s.d, fcvt.d.s
//
// Declared absent: fsqrt.s/d, and the fused multiply-add family
// (fmadd/fmsub/fnmadd/fnmsub) -- the latter are not listed on this card;
// fmadd is named in the capability matrix so that no reader assumes the list is
// complete by omission. Both are reported, never computed. A request for
// `FP_SQRT` (or any value the package does not name) is answered with the
// canonical quiet NaN and NV set -- a marker a later integration can trap on --
// and never with a plausible number.
//
// -------------------------------------------------------- rounding policy
//
// Five modes, in the instruction's own `rm` encoding:
//
//   000 RNE  round to nearest, ties to even            (IEEE default)
//   001 RTZ  round toward zero
//   010 RDN  round toward -infinity
//   011 RUP  round toward +infinity
//   100 RMM  round to nearest, ties to maximum magnitude
//
// 101 and 110 are reserved and 111 is the dynamic mode; resolving 111 from
// `frm`/`fcsr` is the CSR file's job (I-050), not this unit's, so this unit
// receives an already-resolved mode. An unresolved 101/110/111 arriving at
// `req_rm_i` is treated as RNE -- the same treatment for all three, stated
// once here and checked by the testbench -- rather than being a don't-care,
// because a don't-care that later resolves differently is a silent wrong
// answer.
//
// Every operation rounds exactly once. There is no double rounding anywhere:
// division produces a truncated quotient plus a sticky bit and rounds once;
// addition aligns with a sticky bit and rounds once; a result that lands in the
// subnormal range is rounded at the subnormal precision (the IEEE rule) and the
// sticky bit survives the renormalisation.
//
// ---------------------------------------------------------- NaN policy
//
// One canonical quiet NaN, and it is what every NaN-producing operation
// returns:
//
//   single: 0x7FC0_0000   double: 0x7FF8_0000_0000_0000
//
// * No payload is propagated: a NaN operand is not copied into a NaN result.
//   The ISA permits either, and requires the choice to be consistent.
// * A signalling NaN operand sets NV wherever the operation is one that
//   examines its operands' NaN-ness (arithmetic, fmin/fmax, all three
//   comparisons). An operation that does not produce a NaN and does not compare
//   -- fsgnj*, fmv.*, fclass -- never sets NV for a NaN input, and fsgnj/fmv
//   copy the NaN bits through unchanged: they are moves, not arithmetic.
// * feq sets NV only for a signalling NaN input (it is the quiet comparison);
//   flt and fle set NV for *any* NaN input (they are the signalling
//   comparisons). fmin/fmax return the non-NaN operand and set NV only for a
//   signalling NaN operand; with two NaN operands the canonical NaN is
//   returned.
//
// ------------------------------------------------------ subnormal policy
//
// Gradual underflow. There is no flush-to-zero anywhere in this unit on any
// path, and that is a property of the representation rather than a flag:
// operands are unpacked into a normalised (exponent, 64-bit significand) pair
// whose leading one sits at bit 63, so a subnormal *operand* is an ordinary
// small value to every arithmetic path. A subnormal *result* falls out of the
// same rounding step, which rounds at the subnormal precision and sets UF when
// -- and only when -- the exact result is tiny and inexact. Tininess is
// detected *before* rounding: the exact result is tiny when its own exponent is
// below the minimum normal exponent, even if the rounded result reaches the
// smallest normal. IEEE 754-2019 makes the choice of detection an
// implementation decision and requires only consistency; this unit matches the
// host FPU the case is measured against, which was checked on exactly that
// boundary vector (the report's oracle section). An exact subnormal result
// still carries no UF, because UF requires inexactness as well as tininess.
//
// ---------------------------------------------------- the fflags contract
//
// `res_fflags_o` is the five RISC-V flags in their CSR bit order:
//
//   bit 4 NV, bit 3 DZ, bit 2 OF, bit 1 UF, bit 0 NX
//
// so that the consumer can OR them straight into `fcsr.fflags`. **This unit
// only produces the flags of the operation it has just executed.** Making them
// precise -- merging speculative flags and committing them only with the
// instruction that raised them -- is work package I-050's, and this unit's
// header is the boundary: it takes a resolved rounding mode in and hands five
// flag bits out. It holds no architectural FP state (no f0-f31, no fcsr, no
// frm), so it cannot get that boundary wrong.
//
// Flag-by-operation, which the testbench checks per operation rather than on a
// combined "something happened" signal:
//
//   fadd/fsub   NV (inf-inf, sNaN), OF, UF, NX
//   fmul        NV (0*inf, sNaN), OF, UF, NX
//   fdiv        NV (0/0, inf/inf, sNaN), DZ (finite nonzero / 0), OF, UF, NX
//   fsgnj*      none                          fmv.*    none
//   fmin/fmax   NV (sNaN operand only)
//   feq         NV (sNaN operand only)        flt/fle  NV (any NaN operand)
//   fclass      none
//   fcvt int->fp   NX only (OF and UF cannot happen: every integer fits)
//   fcvt fp->int   NV (NaN or out of range, with the RISC-V saturating result
//                  and -- as in the RISC-V reference -- no NX alongside it),
//                  otherwise NX when inexact
//
// Overflow always sets OF and NX together, as IEEE requires, and the delivered
// value depends on the rounding mode (infinity for RNE/RMM; the largest finite
// of the correct sign for RZ; the mode-directed infinity for RUP/RDN).
//
// ------------------------------------------------------- latency contract
//
// Fixed latency per operation class, independent of operand *values* (which is
// the property the RVA23 Zkt work will need, and the reason division always
// runs its full iteration count even when the operands make an early answer
// possible):
//
//   every operation except fdiv   1 cycle
//   fdiv.s/d                     66 cycles  (65 restoring steps, then pack)
//
// "Latency" is measured from the cycle in which `req_valid_i && req_ready_o`
// accepts the request to the cycle in which `res_valid_o` is first high.
// `o_latency_o` states the declared latency of the operation in flight, so the
// testbench compares the declaration against the measurement instead of
// hard-coding either.
//
// ------------------------------------------------------------- handshake
//
//   accept   `req_valid_i && req_ready_o`; `req_ready_o` is high only in IDLE
//            with no flush in that cycle, so at most one operation is in flight
//            and no request is ever accepted and thrown away.
//   hold     the result, its five flag bits and the request's identity are held
//            unchanged until `res_ready_i`, so a consumer that stalls does not
//            lose the answer and a consumer that is being squashed does not
//            receive a value it cannot attribute.
//   tag      the identity (`rob_index`, `rob_gen`, `uop_index`) travels with
//            the request and is returned with the result. The core's completion
//            fabric can consume this exactly as it consumes an ALU or MUL/DIV
//            result; nothing here assumes the consumer is still waiting for the
//            operation it started.
//   flush    absolute priority, one cycle, same contract as mosaic_muldiv: it
//            cancels an operation being computed and a computed result waiting
//            to be taken (counted separately from a partial operation), masks
//            `res_valid_o` in its own cycle, and leaves the unit ready to accept
//            on the next cycle.
//
// Reset clears control state only; every datapath register is written in full
// before it is read (the request is latched before any step reads it), which is
// the rule rtl/common/mosaic_ram.sv states.
//
// Mutation hooks
// --------------
// The shipping build defines none of the `MOSAIC_FPU_MUTANT_*` macros. Each
// injects exactly one broken behaviour so the case can be shown to detect it;
// the mutant table is in results/reports/I-049-fpu.md. They exist to prove the
// test has teeth and have no place in any other build.
// ============================================================================

`default_nettype none
`resetall

/* verilator lint_off UNUSEDPARAM */
/* verilator lint_off UNUSEDSIGNAL */
// Both packages carry their own include guards; see rtl/core/mosaic_muldiv.sv
// for why they are included rather than assumed present on the command line.
`include "mosaic_pkg.sv"
`include "mosaic_id_pkg.svh"
/* verilator lint_on UNUSEDSIGNAL */
/* verilator lint_on UNUSEDPARAM */

localparam int unsigned FPU_ROB_INDEX_W = mosaic_id_pkg::MOSAIC_ID_W_ROB_INDEX;
localparam int unsigned FPU_ROB_GEN_W   = mosaic_id_pkg::MOSAIC_ID_W_ROB_GEN;
localparam int unsigned FPU_UOP_INDEX_W = mosaic_id_pkg::MOSAIC_ID_W_UOP_INDEX;
// Division: 65 restoring steps produce 65 quotient bits, which is more than
// enough for the 53-bit double significand plus guard, round and sticky.
localparam int unsigned FPU_DIV_STEPS = 65;
localparam logic [7:0]   FPU_LAT_DIV   = 8'd66;
localparam logic [7:0]   FPU_LAT_FAST  = 8'd1;

module mosaic_fpu (
    input  logic                        clk_i,
    input  logic                        rst_i,

    // request -- tagged, one operation in flight
    input  logic                        req_valid_i,
    output logic                        req_ready_o,
    input  mosaic_pkg::fp_op_e          req_op_i,
    input  logic                        req_fmt_i,   // 1 = single (S), 0 = double (D)
    input  logic [2:0]                  req_rm_i,    // resolved rounding mode
    input  logic                        req_iw_i,    // cvt: 1 = 64-bit integer side
    input  logic                        req_is_i,    // cvt: 1 = signed integer side
    input  logic [63:0]                 req_a_i,     // rs1 bits (FP or integer)
    input  logic [63:0]                 req_b_i,     // rs2 bits (FP)
    input  logic [FPU_ROB_INDEX_W-1:0]  req_rob_index_i,
    input  logic [FPU_ROB_GEN_W-1:0]    req_rob_gen_i,
    input  logic [FPU_UOP_INDEX_W-1:0]  req_uop_index_i,

    input  logic                        flush_i,

    // response -- the same tag, an explicit ready/valid pair, and the flags
    output logic                        res_valid_o,
    input  logic                        res_ready_i,
    output logic [63:0]                 res_data_o,
    output logic [4:0]                  res_fflags_o,   // {NV,DZ,OF,UF,NX}
    output logic [FPU_ROB_INDEX_W-1:0]  res_rob_index_o,
    output logic [FPU_ROB_GEN_W-1:0]    res_rob_gen_o,
    output logic [FPU_UOP_INDEX_W-1:0]  res_uop_index_o,

    // status and coverage
    output logic                        o_busy,
    output logic [7:0]                  o_latency_o,
    output logic [6:0]                  o_iter,
    output logic [31:0]                 o_accepted_ctr,
    output logic [31:0]                 o_completed_ctr,
    output logic [31:0]                 o_cancelled_ctr,
    output logic [31:0]                 o_killed_res_ctr
);

  // ---------------------------------------------------------------- mutants
`ifdef MOSAIC_FPU_MUTANT_RM_TIE_AWAY
  localparam bit MutTieAway    = 1'b1;   // RNE rounds a tie away from zero
`else
  localparam bit MutTieAway    = 1'b0;
`endif
`ifdef MOSAIC_FPU_MUTANT_DROP_NX
  localparam bit MutDropNx     = 1'b1;   // NX is never set on an inexact result
`else
  localparam bit MutDropNx     = 1'b0;
`endif
`ifdef MOSAIC_FPU_MUTANT_NONCANONICAL_NAN
  localparam bit MutNonCanon   = 1'b1;   // NaN results are not canonical
`else
  localparam bit MutNonCanon   = 1'b0;
`endif
`ifdef MOSAIC_FPU_MUTANT_FLUSH_SUBNORMAL
  localparam bit MutFlushSub   = 1'b1;   // a subnormal result is delivered as zero
`else
  localparam bit MutFlushSub   = 1'b0;
`endif

  // ------------------------------------------------------------------ types
  typedef struct packed {
    logic                sign;
    logic signed [13:0]  exp;     // unbiased exponent of the leading one
    logic [63:0]         mant;    // leading one at bit 63 when nonzero
    logic                is_zero;
    logic                is_sub;
    logic                is_inf;
    logic                is_nan;
    logic                is_snan;
  } fp_unpack_t;

  typedef struct packed {
    logic [63:0] bits;
    logic        nx;
    logic        uf;
    logic        of;
  } fp_round_t;

  typedef struct packed {
    logic [63:0] bits;
    logic        nv;
    logic        dz;
    logic        of;
    logic        uf;
    logic        nx;
  } fp_result_t;

  typedef struct packed {
    logic                special;
    fp_result_t          result;
    logic                sign;
    logic [63:0]         ma;
    logic [63:0]         mb;
    logic signed [13:0]  ea;
    logic signed [13:0]  eb;
  } fp_div_prep_t;

  // ---------------------------------------------------------------- helpers
  //
  // UNUSEDSIGNAL is off for the whole function library below, and the reasons
  // are specific rather than general:
  //
  //   * `fp_unpack_t` is one record shared by every operation, so a function
  //     that does not ask about signalling NaN (fsgnj, fclass, a move) leaves
  //     that field unread -- the field is unused *there*, which is the point of
  //     having one unpack for all of them;
  //   * the 16-bit signed exponent local is the domain the arithmetic works in;
  //     a function whose exponents cannot reach the top two bits leaves them
  //     unread;
  //   * a shift-count local is masked to the bits the shift uses.
  //
  // Each of these is a compiler observation about a local variable, not a
  // design condition: nothing in the unit reads a value that was not assigned.
  // A *port* or a *register* left partly unread would not be suppressed here.
  /* verilator lint_off UNUSEDSIGNAL */
  function automatic logic [63:0] fp_canon_nan(input logic fmt);
    fp_canon_nan = MutNonCanon
        ? (fmt ? 64'h0000_0000_7FC0_0001 : 64'h7FF8_0000_0000_0001)
        : (fmt ? 64'h0000_0000_7FC0_0000 : 64'h7FF8_0000_0000_0000);
  endfunction

  function automatic logic [63:0] fp_inf_bits(input logic sign, input logic fmt);
    fp_inf_bits = fmt ? {32'd0, sign, 8'hFF, 23'd0} : {sign, 11'h7FF, 52'd0};
  endfunction

  function automatic logic [63:0] fp_zero_bits(input logic sign, input logic fmt);
    fp_zero_bits = fmt ? {32'd0, sign, 31'd0} : {sign, 63'd0};
  endfunction

  function automatic logic [63:0] fp_max_bits(input logic sign, input logic fmt);
    fp_max_bits = fmt ? {32'd0, sign, 8'hFE, 23'h7FFFFF}
                      : {sign, 11'h7FE, 52'hF_FFFF_FFFF_FFFF};
  endfunction

  // Replace the sign bit of a packed value. Used where a zero or infinite
  // operand is returned unchanged apart from the sign the operation gives it.
  //
  // For a single-precision value only the low 32 bits are the value, so bits
  // [63:31] of the argument are deliberately not read -- a caller may hand in a
  // 64-bit register whose upper half is NaN-boxing or garbage and the single
  // form must ignore it. Verilator reports that as an unused input bit, which
  // the library-wide suppression above already covers.
  function automatic logic [63:0] fp_with_sign(input logic [63:0] bits,
                                               input logic        sign,
                                               input logic        fmt);
    fp_with_sign = fmt ? {32'd0, sign, bits[30:0]} : {sign, bits[62:0]};
  endfunction

  // Sign-extend a 14-bit biased-out exponent into the 16-bit signed domain the
  // arithmetic uses. Written as an explicit concatenation rather than
  // `$signed(...)`, which does not change a width.
  function automatic logic signed [15:0] fp_sext14(input logic signed [13:0] v);
    fp_sext14 = {{2{v[13]}}, v};
  endfunction

  // ---------------------------------------------------------------- unpack
  function automatic fp_unpack_t fp_unpack(input logic [63:0] bits,
                                           input logic        fmt);
    fp_unpack_t r;
    logic [10:0] exp_raw;
    logic [51:0] frac;
    logic [5:0]  fw;
    logic [6:0]  pos;
    logic        found;
    logic [63:0] tmp;
    begin
      fw      = fmt ? 6'd23 : 6'd52;
      exp_raw = fmt ? {3'b000, bits[30:23]} : bits[62:52];
      frac    = fmt ? {29'd0, bits[22:0]} : bits[51:0];

      r.sign    = fmt ? bits[31] : bits[63];
      r.is_zero = 1'b0;
      r.is_sub  = 1'b0;
      r.is_inf  = 1'b0;
      r.is_nan  = 1'b0;
      r.is_snan = 1'b0;
      r.exp     = 14'sd0;
      r.mant    = 64'd0;

      if (exp_raw == 11'd0) begin
        if (frac == 52'd0) begin
          r.is_zero = 1'b1;
          // A zero needs an exponent that orders it below every non-zero
          // value, because the comparison and min/max paths compare the
          // normalised (exponent, significand) pair. Every real exponent in
          // this unit is above -2200; -4096 is below all of them and, being
          // shared by both zeros with a zero significand, keeps them equal to
          // each other. The sign is handled separately by the callers, which
          // is where the +0/-0 rules live.
          r.exp     = -14'sd4096;
        end else begin
          r.is_sub = 1'b1;
          pos   = 7'd0;
          found = 1'b0;
          for (int i = 51; i >= 0; i--) begin
            if (!found && frac[i]) begin
              pos   = 7'(i);
              found = 1'b1;
            end
          end
          // The leading one of `frac` sits at bit `pos` of the fraction field,
          // so the value is (1 - bias) + pos - fw in the normalised form and
          // the significand is that bit moved up to bit 63.
          tmp    = {12'd0, frac};
          r.mant = tmp << (7'd63 - pos);
          r.exp  = (14'sd1 - (fmt ? 14'sd127 : 14'sd1023))
                   + $signed({8'b0, pos[5:0]}) - $signed({8'b0, fw});
        end
      end else if (exp_raw == (fmt ? 11'd255 : 11'd2047)) begin
        r.is_inf  = (frac == 52'd0);
        r.is_nan  = (frac != 52'd0);
        r.is_snan = (frac != 52'd0) && !frac[fw - 6'd1];
        r.mant    = {12'd0, frac};
      end else begin
        tmp    = {12'd0, frac};
        r.mant = 64'h8000_0000_0000_0000 | (tmp << (7'd63 - {1'b0, fw}));
        r.exp  = $signed({3'b000, exp_raw}) - (fmt ? 14'sd127 : 14'sd1023);
      end
      return r;
    end
  endfunction

  // ------------------------------------------------------- round and pack
  // sign/exp/sig describe an exact nonzero value as 2^exp * sig / 2^64 with
  // sig's leading one at bit 64 and `sticky` holding the OR of every bit below
  // sig[0]. The single rounding step of the unit lives here; every operation
  // that can set OF, UF or NX funnels through it, which is what makes "one
  // rounding per operation" checkable.
  function automatic fp_round_t fp_round_pack(input logic               sign,
                                              input logic signed [13:0] exp,
                                              input logic [65:0]        sig,
                                              input logic               sticky,
                                              input logic               fmt,
                                              input logic [2:0]         rm);
    fp_round_t r;
    logic [5:0]         fw;
    logic signed [15:0] bias_s, exp_s, emin_s, e_eff_s, drop_s, exp_field_s;
    logic signed [15:0] max_s;
    logic [15:0]        drop16;
    logic [6:0]         dsh7;
    logic [6:0]         hidx;
    logic [65:0]        q;
    logic               g, rb, sb, inexact, inc, carry;
    logic [66:0]        sum;
    logic [51:0]        frac;
    begin
      fw        = fmt ? 6'd23 : 6'd52;
      bias_s    = fmt ? 16'sd127 : 16'sd1023;
      exp_s     = fp_sext14(exp);
      emin_s    = 16'sd1 - bias_s;
      max_s     = fmt ? 16'sd255 : 16'sd2047;
      e_eff_s   = (exp_s < emin_s) ? emin_s : exp_s;
      // Bits dropped to reach the destination precision. For a subnormal
      // result the destination precision is the subnormal one, so the shift
      // grows by (emin - exp) -- which is exactly one rounding at the subnormal
      // precision, not a second rounding after a normal one.
      drop_s    = (e_eff_s - exp_s) + (16'sd64 - $signed({10'b0, fw}));
      hidx      = {1'b0, fw};

      if (drop_s >= 16'sd66) begin
        // The value is far below the destination's last place: nothing
        // survives except the sticky knowledge that something was there.
        q  = 66'd0;
        g  = 1'b0;
        rb = 1'b0;
        sb = (|sig) | sticky;
      end else if (drop_s >= 16'sd3) begin
        drop16 = drop_s[15:0];
        dsh7   = drop16[6:0];
        q      = sig >> dsh7;
        g      = sig[dsh7 - 7'd1];
        rb     = sig[dsh7 - 7'd2];
        sb     = (|(sig & ((66'd1 << (dsh7 - 7'd2)) - 66'd1))) | sticky;
      end else if (drop_s == 16'sd2) begin
        q  = sig >> 6'd2;
        g  = sig[1];
        rb = sig[0];
        sb = sticky;
      end else if (drop_s == 16'sd1) begin
        q  = sig >> 6'd1;
        g  = sig[0];
        rb = 1'b0;
        sb = sticky;
      end else begin
        // Unreachable by construction (drop_s >= 64 - fw >= 12), defined so no
        // path can read an unassigned value.
        q  = sig;
        g  = 1'b0;
        rb = 1'b0;
        sb = sticky;
      end

      inexact = g | rb | sb;
      unique case (rm)
        mosaic_pkg::FP_RM_RNE: inc = MutTieAway ? g : (g & (rb | sb | q[0]));
        mosaic_pkg::FP_RM_RTZ: inc = 1'b0;
        mosaic_pkg::FP_RM_RDN: inc = (sign == 1'b1) & inexact;
        mosaic_pkg::FP_RM_RUP: inc = (sign == 1'b0) & inexact;
        mosaic_pkg::FP_RM_RMM: inc = g;
        default:               inc = MutTieAway ? g : (g & (rb | sb | q[0]));
      endcase

      sum   = {1'b0, q} + {66'd0, inc};
      carry = sum[hidx + 7'd1];
      frac  = sum[51:0];

      if (carry) begin
        exp_field_s = e_eff_s - emin_s + 16'sd2;
      end else if (sum[hidx]) begin
        exp_field_s = e_eff_s - emin_s + 16'sd1;
      end else begin
        exp_field_s = 16'sd0;
      end

      if (exp_field_s >= max_s) begin
        // Overflow. IEEE requires OF and NX together, and the delivered value
        // depends on the mode: infinity for the nearest modes, the largest
        // finite for toward-zero, and the mode-directed infinity otherwise.
        r.of = 1'b1;
        r.nx = 1'b1;
        r.uf = 1'b0;
        if ((rm == mosaic_pkg::FP_RM_RTZ) ||
            ((rm == mosaic_pkg::FP_RM_RUP) && (sign == 1'b1)) ||
            ((rm == mosaic_pkg::FP_RM_RDN) && (sign == 1'b0))) begin
          r.bits = fp_max_bits(sign, fmt);
        end else begin
          r.bits = fp_inf_bits(sign, fmt);
        end
      end else begin
        r.of = 1'b0;
        r.nx = MutDropNx ? 1'b0 : inexact;
        // Tininess is detected BEFORE rounding: the exact result is tiny when
        // its own exponent is below the minimum normal exponent, whether or not
        // the rounded result climbs back up to the smallest normal. IEEE
        // 754-2019 leaves the choice of detection to the implementation and
        // requires only that it be consistent; this unit matches the host FPU
        // the case is measured against, which signals underflow for the exact
        // midpoint below the smallest normal even though that midpoint rounds up
        // to it (see the subnormal phase of the driver).
        r.uf = (exp_s < emin_s) & inexact;
        if (MutFlushSub && (exp_field_s == 16'sd0) && inexact) begin
          r.bits = fp_zero_bits(sign, fmt);
        end else if (fmt) begin
          r.bits = {32'd0, sign, exp_field_s[7:0], frac[22:0]};
        end else begin
          r.bits = {sign, exp_field_s[10:0], frac[51:0]};
        end
      end
      return r;
    end
  endfunction

  // --------------------------------------------------------------- add/sub
  function automatic fp_result_t fp_addsub(input logic [63:0] a_bits,
                                           input logic [63:0] b_bits,
                                           input logic        fmt,
                                           input logic [2:0]  rm,
                                           input logic        is_sub);
    fp_result_t r;
    fp_unpack_t ua, ub;
    fp_round_t  rnd;
    logic        sb;
    logic signed [15:0] ea_s, eb_s, ebase_s, diff_s;
    logic [63:0] mhi, mlo, mlo_sh;
    logic [15:0] sh16;
    logic [5:0]  sh;
    logic        a_first, loc;
    logic [65:0] s66, d66, sig;
    logic        sticky;
    logic [6:0]  lz;
    logic        found;
    logic signed [15:0] exp_s;
    logic        sign_res;
    begin
      ua = fp_unpack(a_bits, fmt);
      ub = fp_unpack(b_bits, fmt);
      sb = ub.sign ^ is_sub;

      r.bits = 64'd0; r.nv = 1'b0; r.dz = 1'b0;
      r.of = 1'b0; r.uf = 1'b0; r.nx = 1'b0;

      if (ua.is_nan || ub.is_nan) begin
        r.nv   = ua.is_snan | ub.is_snan;
        r.bits = fp_canon_nan(fmt);
      end else if (ua.is_inf && ub.is_inf) begin
        if (ua.sign == sb) begin
          r.bits = fp_inf_bits(ua.sign, fmt);
        end else begin
          r.nv   = 1'b1;
          r.bits = fp_canon_nan(fmt);
        end
      end else if (ua.is_inf) begin
        r.bits = fp_inf_bits(ua.sign, fmt);
      end else if (ub.is_inf) begin
        r.bits = fp_inf_bits(sb, fmt);
      end else if (ua.is_zero && ub.is_zero) begin
        // +0 and -0: the sum is -0 only when the two signs agree on it, or when
        // rounding downward, which is the one mode whose answer is -0 for an
        // exact cancellation.
        if (ua.sign == sb) begin
          r.bits = fp_zero_bits(ua.sign, fmt);
        end else begin
          r.bits = fp_zero_bits(rm == mosaic_pkg::FP_RM_RDN, fmt);
        end
      end else if (ua.is_zero) begin
        r.bits = fp_with_sign(b_bits, sb, fmt);
      end else if (ub.is_zero) begin
        r.bits = fp_with_sign(a_bits, ua.sign, fmt);
      end else begin
        ea_s = fp_sext14(ua.exp);
        eb_s = fp_sext14(ub.exp);
        diff_s = ea_s - eb_s;
        if (diff_s > 16'sd0) begin
          a_first = 1'b1; ebase_s = ea_s; sh16 = diff_s[15:0];
        end else if (diff_s < 16'sd0) begin
          a_first = 1'b0; ebase_s = eb_s; sh16 = 16'd0 - diff_s[15:0];
        end else begin
          a_first = (ua.mant >= ub.mant); ebase_s = ea_s; sh16 = 16'd0;
        end

        if (a_first) begin
          mhi = ua.mant; mlo = ub.mant;
        end else begin
          mhi = ub.mant; mlo = ua.mant;
        end

        // Align the smaller operand, keeping everything shifted out as one
        // sticky bit: the classic guard/round/sticky arrangement, and the
        // reason one sticky bit is enough for every rounding mode.
        if (sh16 >= 16'd64) begin
          mlo_sh = 64'd0;
          loc    = (mlo != 64'd0);
        end else begin
          sh     = sh16[5:0];
          mlo_sh = mlo >> sh;
          loc    = |(mlo & ((64'd1 << sh) - 64'd1));
        end

        sign_res = a_first ? ua.sign : sb;

        if (ua.sign == sb) begin
          s66    = {2'b00, mhi} + {2'b00, mlo_sh};
          sticky = loc;
          // An aligned significand is a value of `d * 2^(ebase - 63)`, so a
          // leading one at bit 64 makes the exponent ebase + 1 and a leading
          // one at bit 63 leaves it at ebase. Both operands are >= 2^63, so
          // there is nothing further to normalise.
          if (s66[64]) begin
            sig   = s66;
            exp_s = ebase_s + 16'sd1;
          end else begin
            sig   = s66 << 7'd1;
            exp_s = ebase_s;
          end
        end else begin
          // Effective subtraction. The shifted-out bits of the smaller operand
          // reduce the difference's magnitude, so the truncated difference
          // mhi - mlo_sh is an *upper* bound of the exact result. Subtracting
          // `loc` as well turns it into a lower bound and leaves the remainder
          // below the last place, which is the same convention the addition path
          // has for free (there the truncated sum is a lower bound). Without
          // this, the guard/round/sticky bits would describe a value *above*
          // the true one and the directed rounding modes round the wrong way;
          // the case is reachable with ordinary operands (the driver's random
          // phase found it as a one-ulp error under RUP).
          //
          // d66 can still be zero only when the operands are equal, which
          // needs equal exponents and therefore no shift and no `loc`; two
          // operands of a given precision cannot differ by a single bit at
          // bit 63 of the aligned domain.
          d66    = {2'b00, mhi} - {2'b00, mlo_sh} - {65'd0, loc};
          sticky = loc;
          if (d66 == 66'd0) begin
            // Exact cancellation. Equal exponents mean nothing was shifted
            // out, so the difference is exactly zero and the IEEE sign rule
            // applies; unequal exponents cannot cancel to zero because the
            // larger operand keeps its leading one (the argument is in the
            // report).
            r.bits = fp_zero_bits(rm == mosaic_pkg::FP_RM_RDN, fmt);
            sig    = 66'd0;
            exp_s  = ebase_s;
          end else begin
            lz    = 7'd0;
            found = 1'b0;
            for (int i = 65; i >= 0; i--) begin
              if (!found && d66[i]) begin
                lz    = 7'(64 - 7'(i));
                found = 1'b1;
              end
            end
            sig   = d66 << lz;
            exp_s = ebase_s + 16'sd1 - $signed({9'b0, lz});
          end
        end

        if (sig != 66'd0) begin
          rnd    = fp_round_pack(sign_res, exp_s[13:0], sig, sticky, fmt, rm);
          r.bits = rnd.bits;
          r.of   = rnd.of;
          r.uf   = rnd.uf;
          r.nx   = rnd.nx;
        end
      end
      return r;
    end
  endfunction

  // ------------------------------------------------------------------- mul
  function automatic fp_result_t fp_mul(input logic [63:0] a_bits,
                                        input logic [63:0] b_bits,
                                        input logic        fmt,
                                        input logic [2:0]  rm);
    fp_result_t r;
    fp_unpack_t ua, ub;
    fp_round_t  rnd;
    logic [127:0] prod;
    logic [65:0]  sig;
    logic         sticky, sign_res;
    logic signed [15:0] exp_s;
    begin
      ua = fp_unpack(a_bits, fmt);
      ub = fp_unpack(b_bits, fmt);
      sign_res = ua.sign ^ ub.sign;

      r.bits = 64'd0; r.nv = 1'b0; r.dz = 1'b0;
      r.of = 1'b0; r.uf = 1'b0; r.nx = 1'b0;

      if (ua.is_nan || ub.is_nan) begin
        r.nv   = ua.is_snan | ub.is_snan;
        r.bits = fp_canon_nan(fmt);
      end else if ((ua.is_inf && ub.is_zero) || (ua.is_zero && ub.is_inf)) begin
        r.nv   = 1'b1;
        r.bits = fp_canon_nan(fmt);
      end else if (ua.is_inf || ub.is_inf) begin
        r.bits = fp_inf_bits(sign_res, fmt);
      end else if (ua.is_zero || ub.is_zero) begin
        r.bits = fp_zero_bits(sign_res, fmt);
      end else begin
        prod = ua.mant * ub.mant;
        // The product of two significands in [1,2) lies in [1,4): either
        // bit 127 (×2) or bit 126 (×1) is the leading one.
        if (prod[127]) begin
          sig    = {1'b0, prod[127:63]};
          sticky = (|prod[62:0]);
          exp_s  = fp_sext14(ua.exp) + fp_sext14(ub.exp) + 16'sd1;
        end else begin
          sig    = {1'b0, prod[126:62]};
          sticky = (|prod[61:0]);
          exp_s  = fp_sext14(ua.exp) + fp_sext14(ub.exp);
        end
        rnd    = fp_round_pack(sign_res, exp_s[13:0], sig, sticky, fmt, rm);
        r.bits = rnd.bits;
        r.of   = rnd.of;
        r.uf   = rnd.uf;
        r.nx   = rnd.nx;
      end
      return r;
    end
  endfunction

  // ------------------------------------------------------------------- div
  // Special cases and operand normalisation; the restoring loop itself lives in
  // the sequential block below, which is why the result is split in two.
  function automatic fp_div_prep_t fp_div_prep(input logic [63:0] a_bits,
                                               input logic [63:0] b_bits,
                                               input logic        fmt);
    fp_div_prep_t p;
    fp_unpack_t   ua, ub;
    logic         sign_res;
    begin
      ua = fp_unpack(a_bits, fmt);
      ub = fp_unpack(b_bits, fmt);
      sign_res = ua.sign ^ ub.sign;

      p.special     = 1'b0;
      p.result.bits = 64'd0;
      p.result.nv   = 1'b0;
      p.result.dz   = 1'b0;
      p.result.of   = 1'b0;
      p.result.uf   = 1'b0;
      p.result.nx   = 1'b0;
      p.sign        = sign_res;
      p.ma          = ua.mant;
      p.mb          = ub.mant;
      p.ea          = ua.exp;
      p.eb          = ub.exp;

      if (ua.is_nan || ub.is_nan) begin
        p.special     = 1'b1;
        p.result.nv   = ua.is_snan | ub.is_snan;
        p.result.bits = fp_canon_nan(fmt);
      end else if ((ua.is_inf && ub.is_inf) || (ua.is_zero && ub.is_zero)) begin
        p.special     = 1'b1;
        p.result.nv   = 1'b1;
        p.result.bits = fp_canon_nan(fmt);
      end else if (ua.is_inf) begin
        p.special     = 1'b1;
        p.result.bits = fp_inf_bits(sign_res, fmt);
      end else if (ub.is_inf) begin
        p.special     = 1'b1;
        p.result.bits = fp_zero_bits(sign_res, fmt);
      end else if (ub.is_zero) begin
        // Divide by zero is signalled only for a finite nonzero dividend; an
        // infinite dividend over zero is an infinity with no flag, as IEEE
        // states and as the host oracle on this machine does.
        p.special     = 1'b1;
        p.result.dz   = 1'b1;
        p.result.bits = fp_inf_bits(sign_res, fmt);
      end else if (ua.is_zero) begin
        p.special     = 1'b1;
        p.result.bits = fp_zero_bits(sign_res, fmt);
      end
      return p;
    end
  endfunction

  // --------------------------------------------------------- int -> fp cvt
  function automatic fp_result_t fp_cvt_if(input logic [63:0] a_bits,
                                           input logic        fmt,
                                           input logic [2:0]  rm,
                                           input logic        iw,
                                           input logic        is_signed);
    fp_result_t r;
    fp_round_t  rnd;
    logic [63:0] src, mag;
    logic        sign;
    logic [6:0]  pos;
    logic        found;
    logic [65:0] sig;
    begin
      src = iw ? a_bits
               : (is_signed ? {{32{a_bits[31]}}, a_bits[31:0]}
                            : {32'd0, a_bits[31:0]});
      if (is_signed && src[63]) begin
        sign = 1'b1;
        mag  = (~src) + 64'd1;
      end else begin
        sign = 1'b0;
        mag  = src;
      end

      r.bits = 64'd0; r.nv = 1'b0; r.dz = 1'b0;
      r.of = 1'b0; r.uf = 1'b0; r.nx = 1'b0;

      if (mag == 64'd0) begin
        r.bits = fp_zero_bits(1'b0, fmt);
      end else begin
        pos   = 7'd0;
        found = 1'b0;
        for (int i = 63; i >= 0; i--) begin
          if (!found && mag[i]) begin
            pos   = 7'(i);
            found = 1'b1;
          end
        end
        sig = {2'b00, mag} << (7'd64 - pos);
        rnd = fp_round_pack(sign, 14'(pos), sig, 1'b0, fmt, rm);
        r.bits = rnd.bits;
        r.of   = rnd.of;
        r.uf   = rnd.uf;
        r.nx   = rnd.nx;
      end
      return r;
    end
  endfunction

  // --------------------------------------------------------- fp -> int cvt
  function automatic fp_result_t fp_cvt_fi(input logic [63:0] a_bits,
                                           input logic        fmt,
                                           input logic [2:0]  rm,
                                           input logic        iw,
                                           input logic        is_signed);
    fp_result_t r;
    fp_unpack_t ua;
    logic signed [15:0] exp_s, sh_s, shl;
    logic [71:0] q, mag;
    logic        g, rb, sb, inexact, inc;
    logic [63:0] smax, umax;
    begin
      ua    = fp_unpack(a_bits, fmt);
      exp_s = fp_sext14(ua.exp);
      smax  = iw ? 64'h7FFF_FFFF_FFFF_FFFF : 64'h0000_0000_7FFF_FFFF;
      umax  = iw ? 64'hFFFF_FFFF_FFFF_FFFF : 64'h0000_0000_FFFF_FFFF;

      r.bits = 64'd0; r.nv = 1'b0; r.dz = 1'b0;
      r.of = 1'b0; r.uf = 1'b0; r.nx = 1'b0;

      if (ua.is_nan) begin
        r.nv   = 1'b1;
        r.bits = is_signed ? smax : umax;
      end else if (ua.is_inf) begin
        r.nv   = 1'b1;
        if (is_signed) begin
          r.bits = ua.sign ? (iw ? 64'h8000_0000_0000_0000 : 64'hFFFF_FFFF_8000_0000)
                           : smax;
        end else begin
          r.bits = ua.sign ? 64'd0 : umax;
        end
      end else if (ua.is_zero) begin
        r.bits = 64'd0;
      end else if (exp_s > 16'sd70) begin
        // Anything this large is out of range for every destination this unit
        // implements.
        r.nv   = 1'b1;
        if (is_signed) begin
          r.bits = ua.sign ? (iw ? 64'h8000_0000_0000_0000 : 64'hFFFF_FFFF_8000_0000)
                           : smax;
        end else begin
          r.bits = ua.sign ? 64'd0 : umax;
        end
      end else begin
        // The value is 2^(exp-63) * mant with mant's leading one at bit 63, so
        // the integer part is mant >> (63 - exp) and the guard/round/sticky
        // bits are the ones shifted below it.
        if (exp_s >= 16'sd66) begin
          shl = exp_s - 16'sd66;
          q  = {8'b0, ua.mant} << shl[5:0];
          g  = 1'b0;
          rb = 1'b0;
          sb = 1'b0;
        end else if (exp_s == 16'sd65) begin
          q  = {8'b0, ua.mant} << 6'd2;
          g  = 1'b0;
          rb = 1'b0;
          sb = 1'b0;
        end else if (exp_s == 16'sd64) begin
          q  = {8'b0, ua.mant} << 6'd1;
          g  = 1'b0;
          rb = 1'b0;
          sb = 1'b0;
        end else if (exp_s == 16'sd63) begin
          q  = {8'b0, ua.mant};
          g  = 1'b0;
          rb = 1'b0;
          sb = 1'b0;
        end else begin
          sh_s = 16'sd63 - exp_s;
          if (sh_s > 16'sd66) begin
            q  = 72'd0;
            g  = 1'b0;
            rb = 1'b0;
            sb = 1'b1;
          end else if (sh_s == 16'sd66) begin
            q  = 72'd0;
            g  = 1'b0;
            rb = 1'b0;
            sb = 1'b1;
          end else if (sh_s == 16'sd65) begin
            q  = 72'd0;
            g  = 1'b0;
            rb = ua.mant[63];
            sb = (|ua.mant[62:0]);
          end else if (sh_s == 16'sd64) begin
            q  = 72'd0;
            g  = ua.mant[63];
            rb = ua.mant[62];
            sb = (|ua.mant[61:0]);
          end else if (sh_s >= 16'sd3) begin
            q  = {8'b0, ua.mant} >> sh_s[5:0];
            g  = ua.mant[sh_s[5:0] - 6'd1];
            rb = ua.mant[sh_s[5:0] - 6'd2];
            sb = (|(ua.mant & ((64'd1 << (sh_s[5:0] - 6'd2)) - 64'd1)));
          end else if (sh_s == 16'sd2) begin
            q  = {8'b0, ua.mant} >> 6'd2;
            g  = ua.mant[1];
            rb = ua.mant[0];
            sb = 1'b0;
          end else begin
            q  = {8'b0, ua.mant} >> 6'd1;
            g  = ua.mant[0];
            rb = 1'b0;
            sb = 1'b0;
          end
        end

        inexact = g | rb | sb;
        unique case (rm)
          mosaic_pkg::FP_RM_RNE: inc = g & (rb | sb | q[0]);
          mosaic_pkg::FP_RM_RTZ: inc = 1'b0;
          mosaic_pkg::FP_RM_RDN: inc = (ua.sign == 1'b1) & inexact;
          mosaic_pkg::FP_RM_RUP: inc = (ua.sign == 1'b0) & inexact;
          mosaic_pkg::FP_RM_RMM: inc = g;
          default:               inc = g & (rb | sb | q[0]);
        endcase
        mag = q + {71'd0, inc};

        if (!is_signed) begin
          if (ua.sign) begin
            // A negative value is below the range of an unsigned destination;
            // the RISC-V saturation result is the minimum, zero.
            if (mag != 72'd0) r.nv = 1'b1;
            r.bits = 64'd0;
          end else if (iw) begin
            if (mag > 72'h00_FFFF_FFFF_FFFF_FFFF) begin
              r.nv   = 1'b1;
              r.bits = umax;
            end else begin
              r.bits = mag[63:0];
              r.nx   = inexact;
            end
          end else begin
            if (mag > 72'h0000_0000_FFFF_FFFF) begin
              r.nv   = 1'b1;
              r.bits = umax;
            end else begin
              r.bits = {32'd0, mag[31:0]};
              r.nx   = inexact;
            end
          end
        end else begin
          if (ua.sign) begin
            if (mag > (iw ? 72'h8000_0000_0000_0000 : 72'h0000_0000_8000_0000)) begin
              r.nv   = 1'b1;
              r.bits = iw ? 64'h8000_0000_0000_0000 : 64'hFFFF_FFFF_8000_0000;
            end else begin
              r.bits = ~(mag[63:0]) + 64'd1;
              if (!iw) r.bits = {{32{r.bits[31]}}, r.bits[31:0]};
              r.nx   = inexact;
            end
          end else begin
            if (mag > (iw ? 72'h7FFF_FFFF_FFFF_FFFF : 72'h0000_0000_7FFF_FFFF)) begin
              r.nv   = 1'b1;
              r.bits = smax;
            end else begin
              r.bits = mag[63:0];
              if (!iw) r.bits = {32'd0, r.bits[31:0]};
              r.nx   = inexact;
            end
          end
        end
      end
      return r;
    end
  endfunction

  // -------------------------------------------------- fp -> fp format cvt
  function automatic fp_result_t fp_cvt_fmt(input logic [63:0] a_bits,
                                            input logic        fmt_src,
                                            input logic        fmt_dst,
                                            input logic [2:0]  rm);
    fp_result_t r;
    fp_unpack_t ua;
    fp_round_t  rnd;
    logic [65:0] sig;
    begin
      ua = fp_unpack(a_bits, fmt_src);
      r.bits = 64'd0; r.nv = 1'b0; r.dz = 1'b0;
      r.of = 1'b0; r.uf = 1'b0; r.nx = 1'b0;

      if (ua.is_nan) begin
        r.nv   = ua.is_snan;
        r.bits = fp_canon_nan(fmt_dst);
      end else if (ua.is_inf) begin
        r.bits = fp_inf_bits(ua.sign, fmt_dst);
      end else if (ua.is_zero) begin
        r.bits = fp_zero_bits(ua.sign, fmt_dst);
      end else begin
        // The unpacked significand's leading one sits at bit 63; the rounder
        // takes it at bit 64, so the value is shifted up by one, exactly as the
        // arithmetic paths do.
        sig = {1'b0, ua.mant, 1'b0};
        rnd = fp_round_pack(ua.sign, ua.exp, sig, 1'b0, fmt_dst, rm);
        r.bits = rnd.bits;
        r.of   = rnd.of;
        r.uf   = rnd.uf;
        r.nx   = rnd.nx;
      end
      return r;
    end
  endfunction

  // -------------------------------------------------------------- compares
  function automatic logic fp_mag_lt(input fp_unpack_t a, input fp_unpack_t b);
    fp_mag_lt = ($signed(a.exp) < $signed(b.exp)) ||
                (($signed(a.exp) == $signed(b.exp)) && (a.mant < b.mant));
  endfunction

  // a < b for two finite or infinite operands in the same format. Zeros are
  // the caller's case (they compare equal regardless of sign).
  function automatic logic fp_lt(input fp_unpack_t a, input fp_unpack_t b);
    begin
      if (a.is_inf && b.is_inf) begin
        fp_lt = a.sign & ~b.sign;
      end else if (a.is_inf) begin
        fp_lt = a.sign;
      end else if (b.is_inf) begin
        fp_lt = ~b.sign;
      end else if (a.sign != b.sign) begin
        fp_lt = a.sign;
      end else if (a.sign) begin
        fp_lt = ~fp_mag_lt(a, b);
      end else begin
        fp_lt = fp_mag_lt(a, b);
      end
    end
  endfunction

  function automatic logic fp_eq_val(input fp_unpack_t a, input fp_unpack_t b);
    begin
      if (a.is_inf || b.is_inf) begin
        fp_eq_val = a.is_inf & b.is_inf & (a.sign == b.sign);
      end else if (a.is_zero || b.is_zero) begin
        fp_eq_val = a.is_zero & b.is_zero;
      end else begin
        fp_eq_val = (a.sign == b.sign) && (a.exp == b.exp) && (a.mant == b.mant);
      end
    end
  endfunction

  function automatic fp_result_t fp_compare(input logic [63:0] a_bits,
                                            input logic [63:0] b_bits,
                                            input logic        fmt,
                                            input logic [1:0]  kind);
    fp_result_t r;
    fp_unpack_t ua, ub;
    logic lt, eq;
    begin
      ua = fp_unpack(a_bits, fmt);
      ub = fp_unpack(b_bits, fmt);
      r.bits = 64'd0; r.nv = 1'b0; r.dz = 1'b0;
      r.of = 1'b0; r.uf = 1'b0; r.nx = 1'b0;
      if (ua.is_nan || ub.is_nan) begin
        // feq is the quiet comparison: only a signalling NaN operand raises
        // NV. flt and fle are the signalling comparisons: any NaN raises it.
        r.nv   = (kind == 2'd0) ? (ua.is_snan | ub.is_snan) : 1'b1;
        r.bits = 64'd0;
      end else begin
        lt = fp_lt(ua, ub);
        eq = fp_eq_val(ua, ub);
        unique case (kind)
          2'd0:    r.bits = {63'd0, eq};
          2'd1:    r.bits = {63'd0, lt};
          default: r.bits = {63'd0, lt | eq};
        endcase
      end
      return r;
    end
  endfunction

  function automatic fp_result_t fp_classify(input logic [63:0] a_bits,
                                             input logic        fmt);
    fp_result_t r;
    fp_unpack_t ua;
    logic [9:0] mask;
    begin
      ua = fp_unpack(a_bits, fmt);
      r.bits = 64'd0; r.nv = 1'b0; r.dz = 1'b0;
      r.of = 1'b0; r.uf = 1'b0; r.nx = 1'b0;
      mask[0] = ua.is_inf &&  ua.sign;                                  // -inf
      mask[1] = ua.sign && !ua.is_zero && !ua.is_sub && !ua.is_inf && !ua.is_nan;
      mask[2] = ua.sign &&  ua.is_sub;                                  // -subnormal
      mask[3] = ua.sign &&  ua.is_zero;                                 // -0
      mask[4] = !ua.sign && ua.is_zero;                                 // +0
      mask[5] = !ua.sign && ua.is_sub;                                  // +subnormal
      mask[6] = !ua.sign && !ua.is_zero && !ua.is_sub && !ua.is_inf && !ua.is_nan;
      mask[7] = !ua.sign && ua.is_inf;                                  // +inf
      mask[8] = ua.is_nan && ua.is_snan;
      mask[9] = ua.is_nan && !ua.is_snan;
      r.bits  = {54'd0, mask};
      return r;
    end
  endfunction

  function automatic fp_result_t fp_minmax(input logic [63:0] a_bits,
                                           input logic [63:0] b_bits,
                                           input logic        fmt,
                                           input logic        is_min);
    fp_result_t r;
    fp_unpack_t ua, ub;
    logic lt, a_lt_b;
    begin
      ua = fp_unpack(a_bits, fmt);
      ub = fp_unpack(b_bits, fmt);
      r.bits = 64'd0; r.nv = 1'b0; r.dz = 1'b0;
      r.of = 1'b0; r.uf = 1'b0; r.nx = 1'b0;

      if (ua.is_nan && ub.is_nan) begin
        r.nv   = ua.is_snan | ub.is_snan;
        r.bits = fp_canon_nan(fmt);
      end else if (ua.is_nan) begin
        r.nv   = ua.is_snan;
        r.bits = fp_with_sign(b_bits, ub.sign, fmt);
      end else if (ub.is_nan) begin
        r.nv   = ub.is_snan;
        r.bits = fp_with_sign(a_bits, ua.sign, fmt);
      end else if (ua.is_zero && ub.is_zero) begin
        // fmin(-0, +0) is -0 and fmax(-0, +0) is +0: the sign of the result is
        // the OR of the signs for a minimum and the AND for a maximum.
        r.bits = fp_zero_bits(is_min ? (ua.sign | ub.sign) : (ua.sign & ub.sign),
                              fmt);
      end else begin
        if (ua.is_inf && ub.is_inf) begin
          a_lt_b = 1'b0;
        end else if (ua.is_inf) begin
          a_lt_b = ua.sign;
        end else if (ub.is_inf) begin
          a_lt_b = ~ub.sign;
        end else if (ua.sign != ub.sign) begin
          a_lt_b = ua.sign;
        end else if (ua.sign) begin
          a_lt_b = ~fp_mag_lt(ua, ub);
        end else begin
          a_lt_b = fp_mag_lt(ua, ub);
        end
        lt = a_lt_b;
        if (is_min) begin
          r.bits = lt ? fp_with_sign(a_bits, ua.sign, fmt)
                      : fp_with_sign(b_bits, ub.sign, fmt);
        end else begin
          r.bits = lt ? fp_with_sign(b_bits, ub.sign, fmt)
                      : fp_with_sign(a_bits, ua.sign, fmt);
        end
      end
      return r;
    end
  endfunction

  function automatic fp_result_t fp_sgnj(input logic [63:0] a_bits,
                                         input logic [63:0] b_bits,
                                         input logic        fmt,
                                         input logic [1:0]  kind);
    fp_result_t r;
    logic sa, sb;
    begin
      sa = fmt ? a_bits[31] : a_bits[63];
      sb = fmt ? b_bits[31] : b_bits[63];
      r.bits = 64'd0; r.nv = 1'b0; r.dz = 1'b0;
      r.of = 1'b0; r.uf = 1'b0; r.nx = 1'b0;
      unique case (kind)
        2'd0:    r.bits = fp_with_sign(a_bits, sb, fmt);         // fsgnj
        2'd1:    r.bits = fp_with_sign(a_bits, ~sb, fmt);        // fsgnjn
        default: r.bits = fp_with_sign(a_bits, sa ^ sb, fmt);    // fsgnjx
      endcase
      return r;
    end
  endfunction

  function automatic fp_result_t fp_move(input logic [63:0] a_bits,
                                         input logic        fmt,
                                         input logic        to_int);
    fp_result_t r;
    begin
      r.bits = 64'd0; r.nv = 1'b0; r.dz = 1'b0;
      r.of = 1'b0; r.uf = 1'b0; r.nx = 1'b0;
      if (to_int) begin
        // fmv.x.w sign-extends the 32-bit pattern into the integer register, as
        // RV64 specifies; fmv.x.d is the whole register.
        r.bits = fmt ? {{32{a_bits[31]}}, a_bits[31:0]} : a_bits;
      end else begin
        // fmv.w.x takes the low 32 bits. NaN-boxing the upper half is the
        // register file's rule, not this unit's, so the upper bits are zero
        // here and the consumer boxes them.
        r.bits = fmt ? {32'd0, a_bits[31:0]} : a_bits;
      end
      return r;
    end
  endfunction

  // The declared marker for an operation this build does not implement: the
  // canonical quiet NaN with NV set, never a plausible number. See the header.
  function automatic fp_result_t fp_unimplemented(input logic fmt);
    fp_result_t r;
    begin
      r.bits = fp_canon_nan(fmt);
      r.nv   = 1'b1;
      r.dz   = 1'b0;
      r.of   = 1'b0;
      r.uf   = 1'b0;
      r.nx   = 1'b0;
      return r;
    end
  endfunction

  // ---------------------------------------------------- combinational result
  /* verilator lint_on UNUSEDSIGNAL */
  fp_result_t   op_res;
  fp_div_prep_t div_prep;

  always_comb begin : op_dispatch
    unique case (req_op_i)
      mosaic_pkg::FP_ADD:    op_res = fp_addsub(req_a_i, req_b_i, req_fmt_i, req_rm_i, 1'b0);
      mosaic_pkg::FP_SUB:    op_res = fp_addsub(req_a_i, req_b_i, req_fmt_i, req_rm_i, 1'b1);
      mosaic_pkg::FP_MUL:    op_res = fp_mul(req_a_i, req_b_i, req_fmt_i, req_rm_i);
      mosaic_pkg::FP_DIV:    op_res = fp_unimplemented(req_fmt_i);
      mosaic_pkg::FP_SQRT:   op_res = fp_unimplemented(req_fmt_i);
      mosaic_pkg::FP_SGNJ:   op_res = fp_sgnj(req_a_i, req_b_i, req_fmt_i, 2'd0);
      mosaic_pkg::FP_SGNJN:  op_res = fp_sgnj(req_a_i, req_b_i, req_fmt_i, 2'd1);
      mosaic_pkg::FP_SGNJX:  op_res = fp_sgnj(req_a_i, req_b_i, req_fmt_i, 2'd2);
      mosaic_pkg::FP_MIN:    op_res = fp_minmax(req_a_i, req_b_i, req_fmt_i, 1'b1);
      mosaic_pkg::FP_MAX:    op_res = fp_minmax(req_a_i, req_b_i, req_fmt_i, 1'b0);
      mosaic_pkg::FP_CMP_EQ: op_res = fp_compare(req_a_i, req_b_i, req_fmt_i, 2'd0);
      mosaic_pkg::FP_CMP_LT: op_res = fp_compare(req_a_i, req_b_i, req_fmt_i, 2'd1);
      mosaic_pkg::FP_CMP_LE: op_res = fp_compare(req_a_i, req_b_i, req_fmt_i, 2'd2);
      mosaic_pkg::FP_CLASS:  op_res = fp_classify(req_a_i, req_fmt_i);
      mosaic_pkg::FP_MV_X:   op_res = fp_move(req_a_i, req_fmt_i, 1'b1);
      mosaic_pkg::FP_MV_W:   op_res = fp_move(req_a_i, req_fmt_i, 1'b0);
      mosaic_pkg::FP_CVT_FI: op_res = fp_cvt_fi(req_a_i, req_fmt_i, req_rm_i,
                                                req_iw_i, req_is_i);
      mosaic_pkg::FP_CVT_IF: op_res = fp_cvt_if(req_a_i, req_fmt_i, req_rm_i,
                                                req_iw_i, req_is_i);
      mosaic_pkg::FP_CVT_FS: op_res = fp_cvt_fmt(req_a_i, 1'b0, 1'b1, req_rm_i);
      mosaic_pkg::FP_CVT_SF: op_res = fp_cvt_fmt(req_a_i, 1'b1, 1'b0, req_rm_i);
      default:               op_res = fp_unimplemented(req_fmt_i);
    endcase
    div_prep = fp_div_prep(req_a_i, req_b_i, req_fmt_i);
  end

  // ------------------------------------------------------------------ state
  typedef enum logic [1:0] {
    ST_IDLE = 2'd0,
    ST_DIV  = 2'd1,
    ST_DONE = 2'd2
  } state_e;

  state_e state_r;

  logic                fmt_r;
  logic [2:0]          rm_r;

  logic [FPU_ROB_INDEX_W-1:0] id_rob_r;
  logic [FPU_ROB_GEN_W-1:0]   id_gen_r;
  logic [FPU_UOP_INDEX_W-1:0] id_uop_r;

  logic [63:0]  res_data_r;
  logic [4:0]   res_flags_r;
  logic         res_valid_r;
  logic [7:0]   lat_r;

  // division state
  logic                d_special_r;
  logic [63:0]         d_special_bits_r;
  logic [4:0]          d_special_flags_r;
  logic                d_sign_r;
  logic [63:0]         d_mb_r;
  logic signed [13:0]  d_ea_r, d_eb_r;
  logic [63:0]         d_rem_r;
  // The quotient is 66 bits: bit 65 is the leading one, which is known before
  // the loop starts (the significands are both in [1,2), so the ratio is in
  // (1/2, 2)), and bits 64:0 are the restoring loop's own quotient.
  logic [65:0]         d_quot_r;
  logic [6:0]          iter_r;

  logic [31:0] accepted_r, completed_r, cancelled_r, killed_r;

  // ------------------------------------------------------------------ flush
  logic flush_cancel;
  assign flush_cancel = flush_i;
  assign req_ready_o  = (state_r == ST_IDLE) && !flush_cancel;
  assign res_valid_o  = res_valid_r && !flush_cancel;
  assign o_busy       = (state_r != ST_IDLE);

  assign res_data_o      = res_data_r;
  assign res_fflags_o    = res_flags_r;
  assign res_rob_index_o = id_rob_r;
  assign res_rob_gen_o   = id_gen_r;
  assign res_uop_index_o = id_uop_r;
  assign o_latency_o     = (state_r == ST_IDLE) ? 8'd0 : lat_r;
  assign o_iter          = iter_r;

  // --------------------------------------------------- divider step (comb)
  logic [64:0] div_rem_sh;
  logic [64:0] div_rem_n;
  // Bit 65 of the shifted low quotient cannot be set: bits 64:0 hold a partial
  // quotient below 2^65 (the loop's invariant, guaranteed by the accept-time
  // pre-subtraction), and shifting it left by one keeps it below 2^66/2. The
  // top bit of the expression is therefore always zero and is dropped by
  // construction rather than by luck, which is what the suppression records.
  /* verilator lint_off UNUSEDSIGNAL */
  logic [65:0] div_qlo_n;
  /* verilator lint_on UNUSEDSIGNAL */
  logic [65:0] div_quot_n;
  logic        div_ge;

  assign div_ge      = (div_rem_sh >= {1'b0, d_mb_r});
  assign div_rem_sh  = {d_rem_r, 1'b0};
  assign div_rem_n   = div_ge ? (div_rem_sh - {1'b0, d_mb_r}) : div_rem_sh;
  assign div_qlo_n   = {d_quot_r[64:0], div_ge};
  assign div_quot_n  = {d_quot_r[65], div_qlo_n[64:0]};

  // The single rounding of a division: the last restoring step's quotient and
  // remainder, one sticky bit (the remainder is nonzero) and one rounding.
  logic [65:0]        div_sig;
  logic               div_sticky, div_k;
  logic signed [13:0] div_exp_s;

  assign div_k       = div_quot_n[65];
  assign div_sig     = div_k ? {1'b0, div_quot_n[65:1]} : div_quot_n;
  assign div_sticky  = (div_k ? div_quot_n[0] : 1'b0) | (div_rem_n != 65'd0);
  assign div_exp_s   = 14'(fp_sext14(d_ea_r) - fp_sext14(d_eb_r)
                           + (div_k ? 16'sd0 : -16'sd1));

  fp_round_t div_rnd;
  logic [63:0] div_bits;
  fp_result_t  div_final;

  always_comb begin : div_pack
    div_rnd   = fp_round_pack(d_sign_r, div_exp_s, div_sig, div_sticky,
                              fmt_r, rm_r);
    div_bits  = div_rnd.bits;
    if (d_special_r) begin
      div_final.bits = d_special_bits_r;
      div_final.nv   = d_special_flags_r[4];
      div_final.dz   = d_special_flags_r[3];
      div_final.of   = d_special_flags_r[2];
      div_final.uf   = d_special_flags_r[1];
      div_final.nx   = d_special_flags_r[0];
    end else begin
      div_final.bits = div_bits;
      div_final.nv   = 1'b0;
      div_final.dz   = 1'b0;
      div_final.of   = div_rnd.of;
      div_final.uf   = div_rnd.uf;
      div_final.nx   = div_rnd.nx;
    end
  end

  // ------------------------------------------------------------------- FSM
  always_ff @(posedge clk_i) begin
    if (rst_i) begin
      state_r      <= ST_IDLE;
      res_valid_r  <= 1'b0;
      res_data_r   <= 64'd0;
      res_flags_r  <= 5'd0;
      lat_r        <= 8'd0;
      iter_r       <= 7'd0;
      d_rem_r      <= 64'd0;
      d_quot_r     <= 66'd0;
      d_special_r  <= 1'b0;
      d_special_bits_r <= 64'd0;
      d_special_flags_r <= 5'd0;
      accepted_r   <= 32'd0;
      completed_r  <= 32'd0;
      cancelled_r  <= 32'd0;
      killed_r     <= 32'd0;
    end else if (flush_cancel) begin
      // A flush destroys an operation being computed and a computed result
      // waiting to be taken. The card's distinction is kept: the second is a
      // *killed result* and is counted separately.
      if (state_r == ST_DIV) begin
        cancelled_r <= cancelled_r + 32'd1;
      end else if (res_valid_r) begin
        cancelled_r <= cancelled_r + 32'd1;
        killed_r    <= killed_r + 32'd1;
      end
      state_r     <= ST_IDLE;
      res_valid_r <= 1'b0;
      iter_r      <= 7'd0;
      d_rem_r     <= 64'd0;
      d_quot_r    <= 66'd0;
      d_special_r <= 1'b0;
    end else begin
      unique case (state_r)
        ST_IDLE: begin
          if (req_valid_i) begin
            accepted_r <= accepted_r + 32'd1;
            fmt_r <= req_fmt_i;
            rm_r  <= req_rm_i;
            id_rob_r <= req_rob_index_i;
            id_gen_r <= req_rob_gen_i;
            id_uop_r <= req_uop_index_i;
            if (req_op_i == mosaic_pkg::FP_DIV) begin
              state_r      <= ST_DIV;
              iter_r       <= 7'd0;
              d_sign_r     <= div_prep.sign;
              d_ea_r       <= div_prep.ea;
              d_eb_r       <= div_prep.eb;
              d_mb_r       <= div_prep.mb;
              // The restoring loop's invariant needs the running remainder
              // below the divisor, and the dividend's significand can be
              // greater than the divisor's (both are in [1,2)). One
              // pre-subtraction makes it hold, and the quotient's leading one
              // is then known before the loop runs -- which is why bit 65 of
              // the quotient register is loaded here instead of being produced
              // by a step.
              if (div_prep.ma >= div_prep.mb) begin
                d_rem_r  <= div_prep.ma - div_prep.mb;
                d_quot_r <= 66'h2_0000_0000_0000_0000;
              end else begin
                d_rem_r  <= div_prep.ma;
                d_quot_r <= 66'd0;
              end
              d_special_r  <= div_prep.special;
              d_special_bits_r  <= div_prep.result.bits;
              d_special_flags_r <= {div_prep.result.nv, div_prep.result.dz,
                                    div_prep.result.of, div_prep.result.uf,
                                    div_prep.result.nx};
              lat_r        <= FPU_LAT_DIV;
              res_valid_r  <= 1'b0;
            end else begin
              state_r      <= ST_DONE;
              res_data_r   <= op_res.bits;
              res_flags_r  <= {op_res.nv, op_res.dz, op_res.of, op_res.uf,
                               op_res.nx};
              lat_r        <= FPU_LAT_FAST;
              res_valid_r  <= 1'b1;
            end
          end
        end
        ST_DIV: begin
          if (iter_r == 7'(FPU_DIV_STEPS - 1)) begin
            state_r     <= ST_DONE;
            res_data_r  <= div_final.bits;
            res_flags_r <= {div_final.nv, div_final.dz, div_final.of,
                            div_final.uf, div_final.nx};
            res_valid_r <= 1'b1;
          end else begin
            iter_r   <= iter_r + 7'd1;
            d_rem_r  <= div_rem_n[63:0];
            d_quot_r <= div_quot_n;
          end
        end
        ST_DONE: begin
          if (res_ready_i) begin
            completed_r <= completed_r + 32'd1;
            res_valid_r <= 1'b0;
            state_r     <= ST_IDLE;
          end
        end
        default: state_r <= ST_IDLE;
      endcase
    end
  end

  assign o_accepted_ctr   = accepted_r;
  assign o_completed_ctr  = completed_r;
  assign o_cancelled_ctr  = cancelled_r;
  assign o_killed_res_ctr = killed_r;

endmodule : mosaic_fpu

`resetall
`default_nettype wire
