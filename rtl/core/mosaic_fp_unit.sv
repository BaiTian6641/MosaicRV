// ============================================================================
// mosaic_fp_unit -- the shared floating-point unit at the integration boundary
// (work package I-050).
//
// ------------------------------------------------------------------ what it is
//
// I-049 delivered `mosaic_fpu`: a self-contained F/D datapath behind a tagged
// request/response handshake that produces a value and the operation's five
// `fflags` ({NV,DZ,OF,UF,NX}). It holds no architectural state. This module is
// the integration that turns that datapath into a core resource:
//
//   * it consumes a *speculative* grant from cluster 0 -- the same shape the
//     shared MUL/DIV unit consumes -- so the FP operation executes as soon as
//     its sources are ready, out of order with respect to retirement;
//   * it applies the *register-file* rules I-049 explicitly left to its
//     consumer: a single-precision operand that is not NaN-boxed is treated as
//     a NaN, and a single-precision result is NaN-boxed on the way out;
//   * it resolves the instruction's rm field against the architectural
//     `frm` (fcsr[7:5]) when the encoding says "dynamic";
//   * it produces an ordinary `wb_event_t` completion, so the value reaches the
//     physical register file, the wakeup network and the ROB exactly as an ALU
//     or MUL/DIV result does -- and it carries the operation's `fflags` beside
//     the completion for the core to record against the ROB entry.
//
// ------------------------------------------------- why the flags ride beside
//
// The card's precise-`fflags` rule is that an operation's flags become
// architectural only when the operation commits, in program order, and a
// squashed operation contributes nothing. This module produces the flags of a
// *speculative* execution; it never writes `fflags`. The core records the five
// bits against the ROB slot when the completion is taken, and merges them into
// fcsr at retire. So an FP operation that is executed and then squashed has
// produced its flags and they are discarded with its ROB entry -- which is
// exactly the property the case's wrong-path phase requires, and the property
// the mutant MOSAIC_CORE_MUTANT_FFLAGS_EARLY removes.
//
// ---------------------------------------------------------------- NaN-boxing
//
// Registers are 64 bits for both formats. A single-precision value is
// NaN-boxed: bits 63:32 are all ones. The rule this module implements, from the
// F extension:
//
//   * a single-precision *operand* whose upper 32 bits are not all ones is
//     treated as the canonical quiet NaN (the ISA permits any NaN handling that
//     is consistent; this build treats it as a quiet NaN, which is why it raises
//     no NV by itself);
//   * a single-precision *result* is written with the upper 32 bits set to all
//     ones, so every value this machine writes to an f-register is boxed.
//
// `fmv.w.x` is where the two rules meet: as an instruction it writes an
// f-register, so its result is boxed; asked to move an unboxed source it copies
// the canonical NaN's low half, because the source rule ran first.
// ============================================================================

`default_nettype none
`resetall

/* verilator lint_off UNUSEDPARAM */
`include "mosaic_cfg_pkg.svh"
/* verilator lint_on UNUSEDPARAM */
/* verilator lint_off UNUSEDSIGNAL */
`include "mosaic_id_pkg.svh"
/* verilator lint_on UNUSEDSIGNAL */
`include "mosaic_pkg.sv"
`include "mosaic_uop_pkg.sv"

localparam int unsigned FPU_IDX_W   = mosaic_id_pkg::MOSAIC_ID_W_ROB_INDEX;
localparam int unsigned FPU_RGEN_W  = mosaic_id_pkg::MOSAIC_ID_W_ROB_GEN;
localparam int unsigned FPU_UOP_W   = mosaic_id_pkg::MOSAIC_ID_W_UOP_INDEX;
localparam int unsigned FPU_TAG_W   = mosaic_id_pkg::MOSAIC_ID_W_PRF_TAG;
localparam int unsigned FPU_PGEN_W  = mosaic_id_pkg::MOSAIC_ID_W_PRF_GEN;
localparam int unsigned FPU_IGEN_W  = mosaic_cfg_pkg::MOSAIC_INT_PRF_TAG_W;
localparam int unsigned FPU_XLEN    = mosaic_cfg_pkg::MOSAIC_XLEN;

// The canonical single-precision quiet NaN, in the low half of the register.
localparam logic [31:0] FPU_CANON_NAN_S = 32'h7FC0_0000;

module mosaic_fp_unit (
    input  logic                            clk,
    input  logic                            rst,

    // ------------------------------------------- request from cluster 0
    input  logic                            req_valid,
    output logic                            req_ready,
    input  mosaic_pkg::fp_op_e              req_op,
    input  logic                            req_fmt,        // 1 = single
    input  logic [2:0]                      req_rm,
    input  logic                            req_dst_fp,
    input  logic                            req_src1_fp,
    input  logic                            req_src2_fp,
    input  logic                            req_iw,
    input  logic                            req_is,
    input  logic [FPU_XLEN-1:0]             req_a,
    input  logic [FPU_XLEN-1:0]             req_b,
    input  logic [FPU_IDX_W-1:0]            req_rob_index,
    input  logic [FPU_RGEN_W-1:0]           req_rob_gen,
    input  logic [FPU_UOP_W-1:0]            req_uop_index,
    input  logic [FPU_TAG_W-1:0]            req_dst_tag,
    input  logic [FPU_IGEN_W-1:0]           req_dst_gen,

    // The architectural fcsr.frm, for an instruction whose rm field says
    // "dynamic" (111).
    input  logic [2:0]                      frm_i,

    // A redirect or trap: cancel the operation in flight and any completed
    // result not yet taken.
    input  logic                            flush_i,

    // --------------------------------------------- completion (writeback port)
    output mosaic_uop_pkg::wb_event_t       wb_ev,
    output logic                            wb_valid,
    input  logic                            wb_ready,
    // The executed operation's flags, valid with `wb_valid`. The core records
    // them against the ROB entry; this unit never writes fcsr.
    output logic [4:0]                      wb_fflags,
    // "The completed operation was a floating-point instruction that modifies
    // FP state" -- the mstatus.FS dirty predicate, carried with the completion.
    output logic                            wb_modifies_fs,

    // ------------------------------------------------------------- evidence
    output logic                            o_busy,
    output logic [31:0]                     o_issue_ctr,
    output logic [31:0]                     o_commit_ctr,
    output logic [31:0]                     o_flags_ctr
);

  // ------------------------------------------------------- operand boxing
  // A single-precision FP operand that is not NaN-boxed is the canonical quiet
  // NaN. An integer operand (a conversion's source) is never touched: the rule
  // is about the FP register namespace.
  function automatic logic [63:0] unbox(input logic [63:0] v,
                                        input logic     is_fp,
                                        input logic     fmt);
    begin
`ifdef MOSAIC_CORE_MUTANT_FP_NO_UNBOX
      // MUTANT (control for CASE=fp.precise_flags_and_boxing): the NaN-boxing
      // operand rule is not applied, so a single-precision operand whose upper
      // 32 bits are not all ones is treated as a number instead of the
      // canonical quiet NaN.
      unbox = v;
`else
      unbox = v;
      if (is_fp && fmt && (v[63:32] != 32'hFFFF_FFFF)) begin
        unbox = {32'h0000_0000, FPU_CANON_NAN_S};
      end
`endif
    end
  endfunction

  // A single-precision result is boxed: bits 63:32 become all ones.
  function automatic logic [63:0] box(input logic [63:0] v,
                                      input logic     dst_fp,
                                      input logic     fmt);
    begin
`ifdef MOSAIC_CORE_MUTANT_FP_NO_BOX
      // MUTANT (control for CASE=fp.precise_flags_and_boxing): a
      // single-precision result is NOT NaN-boxed, so the machine writes an
      // f-register whose upper half is not all ones -- a value the register-file
      // rule itself would then read back as the canonical NaN.
      box = v;
`else
      box = v;
      if (dst_fp && fmt) begin
        box = {32'hFFFF_FFFF, v[31:0]};
      end
`endif
    end
  endfunction

  // ---------------------------------------------------------- rm resolution
  // rm = 111 is the dynamic mode: the instruction takes fcsr.frm. The I-049 unit
  // already treats the reserved modes 101/110 as RNE; the only field this
  // integration has to resolve is the dynamic one.
  logic [2:0] rm_eff;
  always_comb begin
    rm_eff = (req_rm == 3'b111) ? frm_i : req_rm;
  end

  logic [63:0] a_eff, b_eff;
  always_comb begin
    a_eff = unbox(req_a, req_src1_fp, req_fmt);
    b_eff = unbox(req_b, req_src2_fp, req_fmt);
  end

  // ------------------------------------------------------------ the datapath
  // A redirect or trap cancels the operation in flight and invalidates the
  // latched destination identity, so a squashed operation publishes no
  // writeback. MOSAIC_CORE_MUTANT_FP_SQUASH_WRITES removes that cancellation:
  // the operation completes after the squash and its (now stale) completion is
  // published to the register file.
`ifdef MOSAIC_CORE_MUTANT_FP_SQUASH_WRITES
  localparam logic FPU_FLUSH_ENABLE = 1'b0;
`else
  localparam logic FPU_FLUSH_ENABLE = 1'b1;
`endif
  logic        flush_eff;
  assign flush_eff = flush_i && FPU_FLUSH_ENABLE;

  logic        fpu_req_ready;
  logic        fpu_res_valid;
  logic        fpu_res_ready;
  logic [63:0] fpu_res_data;
  logic [4:0]  fpu_res_fflags;
  logic [FPU_IDX_W-1:0]  fpu_res_idx;
  logic [FPU_RGEN_W-1:0] fpu_res_gen;
  logic [FPU_UOP_W-1:0]  fpu_res_uop;

  assign fpu_res_ready = wb_ready;

/* verilator lint_off PINCONNECTEMPTY */
  mosaic_fpu u_fpu (
      .clk_i           (clk),
      .rst_i           (rst),
      .req_valid_i     (req_valid),
      .req_ready_o     (fpu_req_ready),
      .req_op_i        (req_op),
      .req_fmt_i       (req_fmt),
      .req_rm_i        (rm_eff),
      .req_iw_i        (req_iw),
      .req_is_i        (req_is),
      .req_a_i         (a_eff),
      .req_b_i         (b_eff),
      .req_rob_index_i (req_rob_index),
      .req_rob_gen_i   (req_rob_gen),
      .req_uop_index_i (req_uop_index),
      .flush_i         (flush_eff),
      .res_valid_o     (fpu_res_valid),
      .res_ready_i     (fpu_res_ready),
      .res_data_o      (fpu_res_data),
      .res_fflags_o    (fpu_res_fflags),
      .res_rob_index_o (fpu_res_idx),
      .res_rob_gen_o   (fpu_res_gen),
      .res_uop_index_o (fpu_res_uop),
      .o_busy          (o_busy),
      .o_latency_o     (),
      .o_iter          (),
      .o_accepted_ctr  (),
      .o_completed_ctr (),
      .o_cancelled_ctr (),
      .o_killed_res_ctr()
  );
/* verilator lint_on PINCONNECTEMPTY */

  // The grant is consumed exactly when the datapath accepts it. A grant the
  // datapath did not accept is not a grant: the issue queue holds the entry and
  // offers it again.
  assign req_ready = fpu_req_ready;

  // ------------------------------------------------- destination identity
  // One operation in flight, so one latched destination identity, taken in the
  // accept cycle and held until the completion is handed over. It is what lets
  // the writeback path deliver the result to the rename-allocated tag.
  logic                 dst_valid_q;
  logic [FPU_TAG_W-1:0] dst_tag_q;
  logic [FPU_IGEN_W-1:0] dst_gen_q;
  logic                 dst_fp_q;
  logic                 dst_fmt_q;
  logic                 dst_x0_q;

  always_ff @(posedge clk) begin
    if (rst) begin
      dst_valid_q <= 1'b0;
      dst_tag_q   <= {FPU_TAG_W{1'b0}};
      dst_gen_q   <= {FPU_IGEN_W{1'b0}};
      dst_fp_q    <= 1'b0;
      dst_fmt_q   <= 1'b0;
      dst_x0_q    <= 1'b1;
    end else if (req_valid && req_ready) begin
      dst_valid_q <= 1'b1;
      dst_tag_q   <= req_dst_tag;
      dst_gen_q   <= req_dst_gen;
      dst_fp_q    <= req_dst_fp;
      dst_fmt_q   <= req_fmt;
      // Tag 0 is the "no destination" encoding rename uses for x0 and for an
      // integer-destination instruction whose rd is x0; it names no physical
      // register.
      dst_x0_q    <= (req_dst_tag == {FPU_TAG_W{1'b0}});
    end else if (flush_eff) begin
      dst_valid_q <= 1'b0;
    end
  end

  // -------------------------------------------------------- the completion
  // Formed combinationally from the datapath's response and the latched
  // destination. `value_valid` is low for a destination-less operation, which
  // still owes the ROB an answer.
  always_comb begin
    wb_ev.id.hart      = 1'b0;
    wb_ev.id.rob_index = fpu_res_idx;
    wb_ev.id.rob_gen   = fpu_res_gen;
    wb_ev.id.uop_index = fpu_res_uop;
    wb_ev.dst.tag      = dst_tag_q;
    wb_ev.dst.gen      = {{(FPU_PGEN_W - FPU_IGEN_W){1'b0}}, dst_gen_q};
    wb_ev.dst.x0       = dst_x0_q;
    wb_ev.value_valid  = !dst_x0_q;
    wb_ev.value        = box(fpu_res_data, dst_fp_q, dst_fmt_q);
    wb_ev.exc.valid    = 1'b0;
    wb_ev.exc.cause    = {FPU_XLEN{1'b0}};
    wb_ev.exc.tval     = {FPU_XLEN{1'b0}};
    wb_ev.is_store     = 1'b0;
    wb_ev.is_load      = 1'b0;
  end

  assign wb_valid      = fpu_res_valid && dst_valid_q;
  assign wb_fflags     = fpu_res_fflags;
  assign wb_modifies_fs = 1'b1;

  // ---------------------------------------------------------------- evidence
  always_ff @(posedge clk) begin
    if (rst) begin
      o_issue_ctr  <= 32'd0;
      o_commit_ctr <= 32'd0;
      o_flags_ctr  <= 32'd0;
    end else begin
      if (req_valid && req_ready) begin
        o_issue_ctr <= o_issue_ctr + 32'd1;
      end
      if (wb_valid && wb_ready) begin
        o_commit_ctr <= o_commit_ctr + 32'd1;
      end
      if (wb_valid && wb_ready && (wb_fflags != 5'd0)) begin
        o_flags_ctr <= o_flags_ctr + 32'd1;
      end
    end
  end

endmodule : mosaic_fp_unit

`resetall
`default_nettype wire
