// ============================================================================
// mosaic_cluster -- one execution cluster (work package I-023).
//
// An issue queue, an integer ALU, branch resolution, and the result path. The
// cluster is deliberately one grant wide: the issue queue presents one ready
// uop, the class of that uop decides which execution resource consumes it, and
// the uop leaves the queue only when the resource accepts it (`grant_ready`).
// A grant the unit did not accept is not a grant -- that is I-022's contract and
// `MOSAIC_CLUSTER_MUTANT_IGNORE_GRANT_READY` is the negative control for it.
//
// ------------------------------------------------------------- class routing
//
//   UOP_ALU     the ALU consumes it (combinational) and the result is captured
//               into the result register at that edge.
//   UOP_BRANCH  the comparator and the target unit consume it in the same cycle.
//               The link value (PC+4) is the result when the instruction writes
//               one, and the resolution is raised as a *request* on the redirect
//               port -- the cluster never redirects fetch itself.
//   UOP_MULDIV  the request is handed to the shared iterative unit (one per
//               core, reachable from this cluster's issue queue); the result
//               comes back out of band and is assembled by the core top, not
//               here.
//   anything else is refused and counted. Dispatch does not send the memory or
//   system classes in this package (see mosaic_dispatch.sv), so the refusal is a
//   guard rather than a path: the alternative -- issuing a uop whose unit cannot
//   complete it -- is the silent half-execution the card names.
//
// ------------------------------------------------------- the redirect request
//
// A resolution is registered and held until the redirect arbiter acknowledges
// it. It is emitted for every branch, taken or not: a not-taken branch still has
// to be *accounted* as resolved, and the arbiter's acknowledgement is what
// clears the slot. The request names its macro by the {rob_index, generation}
// the issue queue carries, which is the same identity the ROB knows.
//
// ------------------------------------------------------------ the flush path
//
// A redirect discards this cluster's queue, but `mosaic_iq`'s kill port removes
// *named* macros and everything younger -- there is no "empty yourself" command.
// So the cluster scans its own observation view for a present entry, kills that
// entry's macro with `kill_younger`, and repeats until the queue reports empty.
// Each pass removes at least the named entry and strictly shrinks the queue, so
// the purge terminates. While it runs, the cluster grants nothing.
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

localparam int unsigned CL_XLEN    = mosaic_cfg_pkg::MOSAIC_XLEN;
localparam int unsigned CL_DEPTH   = mosaic_cfg_pkg::MOSAIC_IQ_ENTRIES;
localparam int unsigned CL_IDX_W   = mosaic_id_pkg::MOSAIC_ID_W_ROB_INDEX;
localparam int unsigned CL_RGEN_W  = mosaic_id_pkg::MOSAIC_ID_W_ROB_GEN;
localparam int unsigned CL_TAG_W   = mosaic_id_pkg::MOSAIC_ID_W_PRF_TAG;
localparam int unsigned CL_IGEN_W  = mosaic_cfg_pkg::MOSAIC_INT_PRF_TAG_W;
localparam int unsigned CL_UOP_W   = 3;
localparam int unsigned CL_UOP_ID_W = CL_IDX_W + CL_RGEN_W + CL_UOP_W;
localparam int unsigned CL_IDX_L    = (CL_DEPTH <= 1) ? 1 : $clog2(CL_DEPTH);

module mosaic_cluster (
    input  logic                        clk,
    input  logic                        rst,

    // ------------------------------------------------ dispatch insert (IQ)
    input  logic                        ins_valid,
    output logic                        ins_ready,
    input  logic [CL_UOP_ID_W-1:0]      ins_uop,
    input  mosaic_uop_pkg::uop_meta_t   ins_meta,
    input  logic [CL_XLEN-1:0]          ins_imm,
    input  logic [CL_TAG_W-1:0]         ins_src1_tag,
    input  logic [CL_IGEN_W-1:0]        ins_src1_gen,
    input  logic                        ins_src1_ready,
    input  logic [CL_XLEN-1:0]          ins_src1_val,
    input  logic [CL_TAG_W-1:0]         ins_src2_tag,
    input  logic [CL_IGEN_W-1:0]        ins_src2_gen,
    input  logic                        ins_src2_ready,
    input  logic [CL_XLEN-1:0]          ins_src2_val,
    input  logic [CL_TAG_W-1:0]         ins_dst_tag,
    input  logic [CL_IGEN_W-1:0]        ins_dst_gen,

    // --------------------------------------------- value-visible wakeup in
    input  logic                        wu_valid,
    input  logic [CL_TAG_W-1:0]         wu_tag,
    input  logic [CL_IGEN_W-1:0]        wu_gen,
    input  logic [CL_XLEN-1:0]          wu_val,

    // --------------------------------------------------------- flush (purge)
    input  logic                        flush,
    output logic                        flush_busy,

    // ------------------------------------------------- result to the WB arbiter
    output mosaic_uop_pkg::wb_event_t   wb_ev,
    output logic                        wb_valid,
    input  logic                        wb_ready,

    // ------------------------------------- branch resolution request (out)
    output logic                        redir_req_valid,
    output logic [CL_XLEN-1:0]          redir_req_pc,
    output logic [CL_IDX_W-1:0]         redir_req_rob_index,
    output logic [CL_RGEN_W-1:0]        redir_req_rob_gen,
    output logic                        redir_req_taken,
    input  logic                        redir_req_ack,

    // ------------------------------------------- shared MUL/DIV route (out)
    // Only a cluster with `HAS_MULDIV` drives these; the core top connects them
    // to the one shared unit.
    output logic                        md_req_valid,
    input  logic                        md_req_ready,
    output mosaic_pkg::md_op_e          md_req_op,
    output logic                        md_req_w,
    output logic [CL_XLEN-1:0]          md_req_a,
    output logic [CL_XLEN-1:0]          md_req_b,
    output logic [CL_IDX_W-1:0]         md_req_rob_index,
    output logic [CL_RGEN_W-1:0]        md_req_rob_gen,
    output logic [CL_UOP_W-1:0]         md_req_uop_index,
    output logic [CL_TAG_W-1:0]         md_req_dst_tag,
    output logic [CL_IGEN_W-1:0]        md_req_dst_gen,

    // ------------------------------------------------------------ FP out (I-050)
    // The same grant shape as the MUL/DIV port, to the shared floating-point
    // unit. The operand values are the issue queue's granted source values, which
    // are final: the queue presents only an entry whose sources are ready.
    // `fp_req_dst_fp` says the destination is an f-register, so the FP unit knows
    // whether to NaN-box the result. `fp_req_src1_fp`/`fp_req_src2_fp` say which
    // operands are subject to the single-precision NaN-box rule.
    output logic                        fp_req_valid,
    input  logic                        fp_req_ready,
    output mosaic_pkg::fp_op_e          fp_req_op,
    output logic                        fp_req_fmt,
    output logic [2:0]                  fp_req_rm,
    output logic                        fp_req_dst_fp,
    output logic                        fp_req_src1_fp,
    output logic                        fp_req_src2_fp,
    output logic                        fp_req_iw,
    output logic                        fp_req_is,
    output logic [CL_XLEN-1:0]          fp_req_a,
    output logic [CL_XLEN-1:0]          fp_req_b,
    output logic [CL_IDX_W-1:0]         fp_req_rob_index,
    output logic [CL_RGEN_W-1:0]        fp_req_rob_gen,
    output logic [CL_UOP_W-1:0]         fp_req_uop_index,
    output logic [CL_TAG_W-1:0]         fp_req_dst_tag,
    output logic [CL_IGEN_W-1:0]        fp_req_dst_gen,

    // ------------------------------------------------------------ observation
    output logic [CL_DEPTH-1:0]         o_occupied,
    output logic [31:0]                 o_count,
    output logic                        o_full,
    output logic                        o_dst_conflict,
    output logic [31:0]                 o_ins_total,
    output logic [31:0]                 o_grant_total,
    output logic [31:0]                 o_kill_total,
    output logic                        o_grant_valid,
    output logic [CL_UOP_ID_W-1:0]      o_grant_uop,
    output logic [31:0]                 o_alu_ctr,
    output logic [31:0]                 o_branch_ctr,
    output logic [31:0]                 o_md_ctr,
    output logic [31:0]                 o_refuse_ctr,
    output logic [31:0]                 o_purge_ctr,
    output logic [31:0]                 o_wu_miss_ctr,

    // ---------------------------------------------------------- fabric (I-090)
    // The strategy this cluster's local bypass (I-027) is armed with, and what
    // it did. `o_wu2_matched` is the issue queue's own tally of operands the
    // earlier wakeup resolved; the other three are the bypass unit's, with
    // `o_bp_captured_ctr` counting the producers it tapped (the unit has no
    // captured counter of its own because its registered case reads the
    // per-cycle flag instead).
    input  logic                        fab_dyn,
    output logic [31:0]                 o_wu2_matched,
    output logic [31:0]                 o_bp_captured_ctr,
    output logic [31:0]                 o_bp_unauth_ctr,
    output logic [31:0]                 o_bp_flush_ctr
);

  // -------------------------------------------------------------------- IQ
  logic                    iq_ins_ready;
  logic                    iq_grant_valid;
  logic                    iq_grant_ready;
  logic [CL_UOP_ID_W-1:0]  iq_grant_uop;
  /* verilator lint_off UNUSEDSIGNAL */
  // The meta's memory class bits (mem_size/mem_signed/is_fence/is_fence_i) are
  // not read here: this cluster does not service the memory or system classes
  // (dispatch refuses them), and the grant routing uses the class, PC and the
  // ALU/MUL/branch fields.
  mosaic_uop_pkg::uop_meta_t iq_grant_meta;
  /* verilator lint_on UNUSEDSIGNAL */
  logic [CL_XLEN-1:0]      iq_grant_imm;
  logic [CL_XLEN-1:0]      iq_grant_a;
  logic [CL_XLEN-1:0]      iq_grant_b;
  logic [CL_TAG_W-1:0]     iq_grant_dst_tag;
  logic [CL_IGEN_W-1:0]    iq_grant_dst_gen;
  logic                    iq_kill_valid;
  logic [CL_IDX_W-1:0]     iq_kill_index;
  logic [CL_RGEN_W-1:0]    iq_kill_gen;
  logic                    iq_kill_younger;
  logic [CL_IDX_L-1:0]     iq_obs_index;
  logic                    iq_obs_valid;
  /* verilator lint_off UNUSEDSIGNAL */
  // The purge path names a macro, so only the {rob_index, rob_gen} prefix is
  // read; the uop_index is the field a kill deliberately does not compare.
  logic [CL_UOP_ID_W-1:0]  iq_obs_uop;
  /* verilator lint_on UNUSEDSIGNAL */
  logic [31:0]             iq_wu_miss;
  logic [3:0]              iq_count;
  // IQ outputs this cluster does not read. They are the queue's observation and
  // status surface; declaring them (rather than leaving the pins empty) keeps a
  // future consumer a wiring change and keeps the build free of empty-pin
  // warnings.
  logic [2:0]              iq_unused_grant_index;
  logic [4:0]              iq_unused_age;
  logic [2:0]              iq_unused_alloc_idx;
  logic [31:0]             iq_unused_wu_total;
  logic [31:0]             iq_unused_wu_matched;
  logic [31:0]             iq_unused_wu_dup;
  logic [31:0]             iq_unused_wu_stale;
  logic [4:0]              iq_unused_obs_age;
  logic                    iq_unused_obs_ready;
  logic                    iq_unused_obs_granted;
  logic [CL_TAG_W-1:0]     iq_unused_obs_s1t;
  logic [CL_TAG_W-1:0]     iq_unused_obs_s2t;
  logic [CL_IGEN_W-1:0]    iq_unused_obs_s1g;
  logic [CL_IGEN_W-1:0]    iq_unused_obs_s2g;
  mosaic_uop_pkg::uop_meta_t iq_unused_obs_meta;
  logic [CL_XLEN-1:0]      iq_unused_obs_imm;
  logic [CL_TAG_W-1:0]     iq_unused_obs_dstt;
  logic [CL_TAG_W-1:0]     iq_unused_obs_dstg;
  logic                    iq_unused_obs_s1r;
  logic                    iq_unused_obs_s2r;
  logic [CL_XLEN-1:0]      iq_unused_obs_s1v;
  logic [CL_XLEN-1:0]      iq_unused_obs_s2v;

  // ------------------------------------------------------------------ purge FSM
  localparam logic [1:0] ST_RUN   = 2'd0;
  localparam logic [1:0] ST_SCAN  = 2'd1;
  localparam logic [1:0] ST_KILL  = 2'd2;

  logic [1:0]          purge_state;
  logic [CL_IDX_L-1:0] scan_i;
  logic [CL_IDX_W-1:0] kill_idx_q;
  logic [CL_RGEN_W-1:0] kill_gen_q;

  logic purge_kill;

  always_comb begin
    iq_obs_index = scan_i;
  end

  assign flush_busy = (purge_state != ST_RUN);

  assign iq_kill_valid   = purge_kill;
  assign iq_kill_index   = kill_idx_q;
  assign iq_kill_gen     = kill_gen_q;
  assign iq_kill_younger = 1'b1;

  // ------------------------------------------------------------------- ALU
  logic [CL_XLEN-1:0] alu_result;
  logic               alu_zero_unused;

  mosaic_alu #(.XLEN(CL_XLEN)) u_alu (
      .a      (iq_grant_a),
      .b      (iq_grant_b),
      .op     (iq_grant_meta.alu_op),
      .result (alu_result),
      .zero   (alu_zero_unused)
  );

  // ------------------------------------------------------- branch resolution
  /* verilator lint_off UNUSEDSIGNAL */
  // br_sum[0] is deliberately not read: a JALR's bit 0 is cleared, which is the
  // whole of the alignment rule the unit also implements for its own arm.
  logic [CL_XLEN-1:0] br_sum;
  /* verilator lint_on UNUSEDSIGNAL */
  logic [CL_XLEN-1:0] br_target_eff;
  logic               br_taken;
  logic [CL_XLEN-1:0] br_link;
  logic [CL_XLEN-1:0] br_target;
  logic               br_is_taken;

  mosaic_branch_cmp #(.XLEN(CL_XLEN)) u_bcmp (
      .rs1_value    (iq_grant_a),
      .rs2_value    (iq_grant_b),
      .branch_funct (iq_grant_meta.br_funct),
      .taken        (br_taken)
  );

  // A JALR's target base is a *register* (rs1), not the PC, and
  // mosaic_branch_target has a single `pc` input that it adds the immediate to
  // and from which it also derives `link = pc + 4`. One input cannot be both,
  // so the unit is fed the instruction's own PC (keeping `link` and the
  // PC-relative arms exactly as documented) and the JALR arm is computed here,
  // where the rs1 operand is; `is_taken` still comes from the unit, because the
  // predicate does not depend on the base.
  mosaic_branch_target #(.XLEN(CL_XLEN)) u_btgt (
      .pc           (iq_grant_meta.pc),
      // I-041: the link value and the sequential next PC are `pc + length`, and
      // the length is the delivered instruction's (2 for a compressed jump).
      .insn_len     (iq_grant_meta.insn_len),
      .imm          (iq_grant_imm),
      .is_branch    (iq_grant_meta.class_ == mosaic_uop_pkg::UOP_BRANCH &&
                     !iq_grant_meta.is_jal && !iq_grant_meta.is_jalr),
      .is_jal       (iq_grant_meta.is_jal),
      .is_jalr      (iq_grant_meta.is_jalr),
      .branch_taken (br_taken),
      .link         (br_link),
      .target       (br_target),
      .is_taken     (br_is_taken)
  );

  always_comb begin
    br_sum        = iq_grant_a + iq_grant_imm;
    br_target_eff = iq_grant_meta.is_jalr ? {br_sum[CL_XLEN-1:1], 1'b0}
                                          : br_target;
  end

  // --------------------------------------------------------- result register
  mosaic_uop_pkg::wb_event_t wb_ev_q;
  logic                      wb_v_q;

  logic res_free;
  assign res_free  = !wb_v_q || wb_ready;

  assign wb_valid  = wb_v_q;
  assign wb_ev     = wb_ev_q;

  // ------------------------------------------------------ redirect request reg
  logic              rr_v_q;
  logic [CL_XLEN-1:0] rr_pc_q;
  logic [CL_IDX_W-1:0] rr_idx_q;
  logic [CL_RGEN_W-1:0] rr_gen_q;
  logic              rr_taken_q;

  logic rr_free;
  assign rr_free = !rr_v_q || redir_req_ack;

  assign redir_req_valid     = rr_v_q;
  assign redir_req_pc        = rr_pc_q;
  assign redir_req_rob_index = rr_idx_q;
  assign redir_req_rob_gen   = rr_gen_q;
  assign redir_req_taken     = rr_taken_q;

  // ----------------------------------------------------------------- routing
  logic is_alu, is_branch, is_md, is_fp, is_other;
  logic unit_accepts;

  always_comb begin
    is_alu    = (iq_grant_meta.class_ == mosaic_uop_pkg::UOP_ALU);
    is_branch = (iq_grant_meta.class_ == mosaic_uop_pkg::UOP_BRANCH);
    is_md     = (iq_grant_meta.class_ == mosaic_uop_pkg::UOP_MULDIV);
    is_fp     = (iq_grant_meta.class_ == mosaic_uop_pkg::UOP_FP);
    is_other  = !(is_alu || is_branch || is_md || is_fp);
  end

  // The resource accepts only when it can take the result. The ALU and the
  // branch resolver produce their answer combinationally, so "can take" is the
  // result register being free (and, for a branch, the request slot too). The
  // shared units state their own readiness.
  always_comb begin
    unit_accepts = 1'b0;
    if (is_alu) begin
      unit_accepts = res_free;
    end else if (is_branch) begin
      unit_accepts = res_free && rr_free;
    end else if (is_md) begin
      unit_accepts = md_req_ready;
    end else if (is_fp) begin
      unit_accepts = fp_req_ready;
    end else begin
      unit_accepts = 1'b0;   // refused: counted, never silently executed
    end
  end

  // A grant is consumed only when the receiving unit accepts it. No purge grant.
  assign iq_grant_ready = (purge_state == ST_RUN) && unit_accepts;
  assign ins_ready      = iq_ins_ready && (purge_state == ST_RUN);

  logic grant_fire;
  assign grant_fire = iq_grant_valid && iq_grant_ready;

  // ------------------------------------------------------------- MUL/DIV out
  assign md_req_valid     = grant_fire && is_md;
  assign md_req_op        = iq_grant_meta.md_op;
  assign md_req_w         = iq_grant_meta.md_w;
  assign md_req_a         = iq_grant_a;
  assign md_req_b         = iq_grant_b;
  assign md_req_rob_index = iq_grant_uop[CL_UOP_ID_W-1 -: CL_IDX_W];
  assign md_req_rob_gen   = iq_grant_uop[CL_IGEN_W + CL_UOP_W - 1 -: CL_RGEN_W];
  assign md_req_uop_index = iq_grant_uop[CL_UOP_W-1:0];
  assign md_req_dst_tag   = iq_grant_dst_tag;
  assign md_req_dst_gen   = iq_grant_dst_gen;

  // -------------------------------------------------------------- FP out
  assign fp_req_valid     = grant_fire && is_fp;
  assign fp_req_op        = iq_grant_meta.fp_op;
  assign fp_req_fmt       = iq_grant_meta.fp_fmt;
  assign fp_req_rm        = iq_grant_meta.fp_rm;
  assign fp_req_dst_fp    = iq_grant_meta.fp_dst_fp;
  assign fp_req_src1_fp   = iq_grant_meta.fp_src1_fp;
  assign fp_req_src2_fp   = iq_grant_meta.fp_src2_fp;
  assign fp_req_iw        = iq_grant_meta.fp_iw;
  assign fp_req_is        = iq_grant_meta.fp_is;
  assign fp_req_a         = iq_grant_a;
  assign fp_req_b         = iq_grant_b;
  assign fp_req_rob_index = iq_grant_uop[CL_UOP_ID_W-1 -: CL_IDX_W];
  assign fp_req_rob_gen   = iq_grant_uop[CL_IGEN_W + CL_UOP_W - 1 -: CL_RGEN_W];
  assign fp_req_uop_index = iq_grant_uop[CL_UOP_W-1:0];
  assign fp_req_dst_tag   = iq_grant_dst_tag;
  assign fp_req_dst_gen   = iq_grant_dst_gen;

  // ------------------------------------------------------------- counters
  logic [31:0] alu_ctr, branch_ctr, md_ctr, refuse_ctr, purge_ctr;

  always_ff @(posedge clk) begin
    if (rst) begin
      alu_ctr    <= 32'd0;
      branch_ctr <= 32'd0;
      md_ctr     <= 32'd0;
      refuse_ctr <= 32'd0;
      purge_ctr  <= 32'd0;
    end else begin
      if (grant_fire && is_alu)    alu_ctr    <= alu_ctr + 32'd1;
      if (grant_fire && is_branch) branch_ctr <= branch_ctr + 32'd1;
      if (grant_fire && is_md)     md_ctr     <= md_ctr + 32'd1;
      if (iq_grant_valid && is_other && (purge_state == ST_RUN))
        refuse_ctr <= refuse_ctr + 32'd1;
      if (purge_kill) purge_ctr <= purge_ctr + 32'd1;
    end
  end

  // --------------------------------------------------------- result capture
  always_ff @(posedge clk) begin
    if (rst) begin
      wb_v_q  <= 1'b0;
      rr_v_q  <= 1'b0;
      rr_pc_q <= {CL_XLEN{1'b0}};
      rr_idx_q <= {CL_IDX_W{1'b0}};
      rr_gen_q <= {CL_RGEN_W{1'b0}};
      rr_taken_q <= 1'b0;
    end else begin
      // The result register drains on its handshake; a new result is captured in
      // the same cycle the previous one leaves.
      if (wb_v_q && wb_ready) begin
        wb_v_q <= 1'b0;
      end
      if (grant_fire && (is_alu || is_branch)) begin
        wb_ev_q.dst.tag   <= iq_grant_dst_tag;
        wb_ev_q.dst.gen   <= {{(mosaic_uop_pkg::GEN_W - CL_IGEN_W){1'b0}},
                              iq_grant_dst_gen};
        // `dst_tag == 0` is "no physical destination": rename's free list never
        // contains tag 0 (it is the committed mapping of x0 and is never
        // released), so a uop can never be allocated it. A uop that writes no
        // register is flagged x0 here, which is the same statement the arbiter
        // needs: nothing may be written.
        wb_ev_q.dst.x0    <= (iq_grant_dst_tag == {CL_TAG_W{1'b0}});
        wb_ev_q.value_valid <= (iq_grant_dst_tag != {CL_TAG_W{1'b0}}) &&
                               (is_alu || iq_grant_meta.writes_link);
        wb_ev_q.value     <= is_branch ? br_link : alu_result;
        wb_ev_q.exc.valid <= 1'b0;
        wb_ev_q.exc.cause <= {CL_XLEN{1'b0}};
        wb_ev_q.exc.tval  <= {CL_XLEN{1'b0}};
        wb_ev_q.is_store  <= 1'b0;
        wb_ev_q.is_load   <= 1'b0;
        wb_ev_q.id.hart      <= 1'b0;
        wb_ev_q.id.rob_index <= iq_grant_uop[CL_UOP_ID_W-1 -: CL_IDX_W];
        wb_ev_q.id.rob_gen   <= iq_grant_uop[CL_IGEN_W + CL_UOP_W - 1 -: CL_RGEN_W];
        wb_ev_q.id.uop_index <= iq_grant_uop[CL_UOP_W-1:0];
        wb_v_q <= 1'b1;
      end

      // A branch resolution is always raised (taken or not): the arbiter's
      // acknowledgement is what accounts for it and frees the slot.
      if (rr_v_q && redir_req_ack) begin
        rr_v_q <= 1'b0;
      end
      if (grant_fire && is_branch) begin
        rr_pc_q    <= br_target_eff;
        rr_idx_q   <= iq_grant_uop[CL_UOP_ID_W-1 -: CL_IDX_W];
        rr_gen_q   <= iq_grant_uop[CL_IGEN_W + CL_UOP_W - 1 -: CL_RGEN_W];
`ifdef MOSAIC_CLUSTER_MUTANT_IGNORE_TAKEN
        // NEGATIVE CONTROL: the resolution is raised as not taken whatever the
        // comparator said. The arbiter then never redirects, the fall-through
        // path keeps running, and the wrong-path instructions retire:
        // CASE=core.corpus_branch must fail on the first instruction after a
        // taken branch. This is the "a branch's resolution is not taken into
        // account" control.
        rr_taken_q <= 1'b0;
`else
        rr_taken_q <= br_is_taken;
`endif
        rr_v_q     <= 1'b1;
      end
    end
  end

  // ------------------------------------------------------------ purge FSM
  always_comb begin
    purge_kill = (purge_state == ST_KILL);
  end

  always_ff @(posedge clk) begin
    if (rst) begin
      purge_state <= ST_RUN;
      scan_i      <= {CL_IDX_L{1'b0}};
      kill_idx_q  <= {CL_IDX_W{1'b0}};
      kill_gen_q  <= {CL_RGEN_W{1'b0}};
    end else begin
      case (purge_state)
        ST_RUN: begin
          if (flush) begin
            purge_state <= ST_SCAN;
            scan_i      <= {CL_IDX_L{1'b0}};
          end
        end
        ST_SCAN: begin
          if (iq_obs_valid) begin
            // Latch the identity of the first present entry, then kill it and
            // everything younger on the next cycle.
            kill_idx_q  <= iq_obs_uop[CL_UOP_ID_W-1 -: CL_IDX_W];
            kill_gen_q  <= iq_obs_uop[CL_IGEN_W + CL_UOP_W - 1 -: CL_RGEN_W];
            purge_state <= ST_KILL;
          end else if (scan_i == CL_IDX_L'(CL_DEPTH - 1)) begin
            purge_state <= ST_RUN;   // nothing left: the queue is empty
          end else begin
            scan_i <= scan_i + CL_IDX_L'(1);
          end
        end
        ST_KILL: begin
          // The kill removes at least the named entry, so a rescan strictly
          // shrinks the queue and the loop terminates.
          purge_state <= ST_SCAN;
          scan_i      <= {CL_IDX_L{1'b0}};
        end
        default: purge_state <= ST_RUN;
      endcase
    end
  end

  // ==========================================================================
  // The local bypass (I-027), wired by I-090
  // ==========================================================================
  // A consumer of an ALU or branch result does not have to wait for the durable
  // writeback broadcast. The tap is the functional unit's result *before* the
  // cluster's result register, registered once in the bypass slot; the slot's
  // identity and value are offered to the issue queue as a second, earlier
  // value-visible wakeup (see mosaic_iq's `wu2_*`). Every rule the unit states
  // is kept: the producer must write a value and not be a squashed macro (a
  // redirect clears the slot before it can forward), and the durable path is
  // untouched -- the later broadcast for the same identity is still delivered
  // and is still the duplicate the issue queue already ignores.
  //
  // Armed by the fabric strategy. With it low the slot is emptied and the wakeup
  // port is idle, so the cluster is the pre-I-090 machine.
  logic                    bp_p_valid;
  logic                    bp_src_valid;
  logic [CL_TAG_W-1:0]     bp_src_tag;
  // The bypass's generation width is the PRF identity's (8), one bit wider
  // than the issue queue's integer namespace (7); the extra bit is always 0.
  /* verilator lint_off UNUSEDSIGNAL */
  logic [mosaic_id_pkg::MOSAIC_ID_W_PRF_GEN-1:0] bp_src_gen;
  /* verilator lint_on UNUSEDSIGNAL */
  logic [CL_XLEN-1:0]      bp_src_value;
  logic                    bp_wu2_valid;
  logic                    bp_captured;
  logic [31:0]             bp_captured_ctr;
  /* verilator lint_off UNUSEDSIGNAL */
  // The unit's per-candidate operand-resolution ports. This core's issue queue
  // resolves operands from *wakeups*, so the cluster takes the bypass as the
  // earlier wakeup above and does not present a candidate to the per-candidate
  // ports; they remain the module's own contract and CASE=bypass.local_raw_chain
  // is their evidence. The port must be driven, so these are declared and left.
  logic                    bp_s1_hit, bp_s2_hit, bp_fb_s1_sel, bp_fb_s2_sel;
  logic                    bp_s1_rdy, bp_s2_rdy, bp_s1_src, bp_s2_src;
  logic [CL_XLEN-1:0]      bp_s1_val, bp_s2_val;
  logic                    bp_slot_valid;
  logic [CL_TAG_W-1:0]     bp_slot_tag;
  logic [mosaic_id_pkg::MOSAIC_ID_W_PRF_GEN-1:0] bp_slot_gen;
  logic [CL_IDX_W-1:0]     bp_slot_rob_index;
  logic [CL_RGEN_W-1:0]    bp_slot_rob_gen;
  logic [CL_UOP_W-1:0]     bp_slot_uop_index;
  logic                    bp_unauth;
  logic [31:0]             bp_hit_ctr, bp_miss_ctr, bp_id_reject_ctr;
  logic [1:0]              bp_s1_src_n, bp_s2_src_n;
  /* verilator lint_on UNUSEDSIGNAL */

  // The tap: a value-writing ALU or branch macro, in the cycle its functional
  // unit computed it, and before the result register. A branch that writes no
  // link and any uop with no destination contribute nothing.
  assign bp_p_valid = grant_fire &&
                      (iq_grant_dst_tag != {CL_TAG_W{1'b0}}) &&
                      (is_alu || (is_branch && iq_grant_meta.writes_link));

  mosaic_cluster_bypass u_bypass (
      .clk            (clk),
      .rst            (rst),
      .bp_en          (fab_dyn),
      .p_valid        (bp_p_valid),
      // Every grant this cluster issues belongs to an allocated, unsquashed
      // macro: the issue queue removes a killed macro, and a redirect purges
      // the queue before a stale macro can be granted. The unit's identity
      // discipline (a withdrawn grant is never forwarded) is therefore not
      // narrowed here, and its own case drives the refusal directly.
      .p_authorised   (1'b1),
      .p_tag          (iq_grant_dst_tag),
      .p_gen          (mosaic_id_pkg::MOSAIC_ID_W_PRF_GEN'(iq_grant_dst_gen)),
      .p_rob_index    (iq_grant_uop[CL_UOP_ID_W-1 -: CL_IDX_W]),
      .p_rob_gen      (iq_grant_uop[CL_IGEN_W + CL_UOP_W - 1 -: CL_RGEN_W]),
      .p_uop_index    (iq_grant_uop[CL_UOP_W-1:0]),
      .p_value        (is_branch ? br_link : alu_result),
      .flush          (flush),
      // The durable broadcast, so the unit's own fallback rule is the same one
      // the queue already takes.
      .w_valid        (wu_valid),
      .w_tag          (wu_tag),
      .w_gen          (mosaic_id_pkg::MOSAIC_ID_W_PRF_GEN'(wu_gen)),
      .w_val          (wu_val),
      // No candidate is presented (see the note above).
      .c_valid        (1'b0),
      .c_s1_tag       ({CL_TAG_W{1'b0}}),
      .c_s1_gen       (mosaic_id_pkg::MOSAIC_ID_W_PRF_GEN'(1'b0)),
      .c_s1_need      (1'b0),
      .c_s2_tag       ({CL_TAG_W{1'b0}}),
      .c_s2_gen       (mosaic_id_pkg::MOSAIC_ID_W_PRF_GEN'(1'b0)),
      .c_s2_need      (1'b0),
      .bp_s1_hit      (bp_s1_hit),
      .bp_s2_hit      (bp_s2_hit),
      .fb_s1_sel      (bp_fb_s1_sel),
      .fb_s2_sel      (bp_fb_s2_sel),
      .s1_rdy         (bp_s1_rdy),
      .s2_rdy         (bp_s2_rdy),
      .s1_val         (bp_s1_val),
      .s2_val         (bp_s2_val),
      .s1_src         (bp_s1_src_n),
      .s2_src         (bp_s2_src_n),
      .slot_valid     (bp_slot_valid),
      .slot_tag       (bp_slot_tag),
      .slot_gen       (bp_slot_gen),
      .slot_rob_index (bp_slot_rob_index),
      .slot_rob_gen   (bp_slot_rob_gen),
      .slot_uop_index (bp_slot_uop_index),
      .bp_src_valid   (bp_src_valid),
      .bp_src_tag     (bp_src_tag),
      .bp_src_gen     (bp_src_gen),
      .bp_src_value   (bp_src_value),
      .o_slot_captured(bp_captured),
      .o_unauth       (bp_unauth),
      .o_hit_ctr      (bp_hit_ctr),
      .o_miss_ctr     (bp_miss_ctr),
      .o_unauth_ctr   (o_bp_unauth_ctr),
      .o_id_reject_ctr(bp_id_reject_ctr),
      .o_flush_ctr    (o_bp_flush_ctr)
  );

  assign bp_wu2_valid = fab_dyn && bp_src_valid;

  always_ff @(posedge clk) begin
    if (rst) begin
      bp_captured_ctr <= 32'd0;
    end else if (bp_captured) begin
      bp_captured_ctr <= bp_captured_ctr + 32'd1;
    end
  end
  assign o_bp_captured_ctr = bp_captured_ctr;

  // ------------------------------------------------------------------- IQ
  // mosaic_iq declares its geometry as `localparam` entries in its parameter
  // port list, so it cannot be overridden and is not: the generated package is
  // the only source of the geometry on both sides.
  mosaic_iq u_iq (
      .clk             (clk),
      .rst             (rst),

      .ins_valid       (ins_valid),
      .ins_ready       (iq_ins_ready),
      .ins_uop         (ins_uop),
      .ins_meta        (ins_meta),
      .ins_imm         (ins_imm),
      .ins_src1_tag    (ins_src1_tag),
      .ins_src1_gen    (ins_src1_gen),
      .ins_src1_ready  (ins_src1_ready),
      .ins_src1_val    (ins_src1_val),
      .ins_src2_tag    (ins_src2_tag),
      .ins_src2_gen    (ins_src2_gen),
      .ins_src2_ready  (ins_src2_ready),
      .ins_src2_val    (ins_src2_val),
      .ins_dst_tag     (ins_dst_tag),
      .ins_dst_gen     (ins_dst_gen),

      .wu_valid        (wu_valid),
      .wu_tag          (wu_tag),
      .wu_gen          (wu_gen),
      .wu_val          (wu_val),

      // The local bypass's earlier wakeup (I-090). Armed by the strategy; the
      // queue applies it with the durable port's own rules, so a later broadcast
      // for the same identity is still the duplicate it already ignores.
      .wu2_valid       (bp_wu2_valid),
      .wu2_tag         (bp_src_tag),
      .wu2_gen         (bp_src_gen[CL_IGEN_W-1:0]),
      .wu2_val         (bp_src_value),

      .grant_valid     (iq_grant_valid),
      .grant_ready     (iq_grant_ready),
      .grant_uop       (iq_grant_uop),
      .grant_meta      (iq_grant_meta),
      .grant_imm       (iq_grant_imm),
      .grant_a         (iq_grant_a),
      .grant_b         (iq_grant_b),
      .grant_dst_tag   (iq_grant_dst_tag),
      .grant_dst_gen   (iq_grant_dst_gen),
      .grant_index     (iq_unused_grant_index),

      .kill_valid      (iq_kill_valid),
      .kill_rob_index  (iq_kill_index),
      .kill_rob_gen    (iq_kill_gen),
      .kill_younger    (iq_kill_younger),

      .o_occupied      (o_occupied),
      .o_count         (iq_count),
      .o_full          (o_full),
      .o_dst_conflict  (o_dst_conflict),
      .o_age_ctr       (iq_unused_age),
      .o_alloc_index   (iq_unused_alloc_idx),

      // The per-slot observation view is only read by the purge path, through
      // obs_index/obs_valid/obs_uop. The remaining fields are part of the
      // queue's verification surface and are not consumed here.
      .obs_index       (iq_obs_index),
      .obs_valid       (iq_obs_valid),
      .obs_age         (iq_unused_obs_age),
      .obs_ready       (iq_unused_obs_ready),
      .obs_granted     (iq_unused_obs_granted),
      .obs_src1_tag    (iq_unused_obs_s1t),
      .obs_src1_gen    (iq_unused_obs_s1g),
      .obs_src2_tag    (iq_unused_obs_s2t),
      .obs_src2_gen    (iq_unused_obs_s2g),
      .obs_uop         (iq_obs_uop),
      .obs_meta        (iq_unused_obs_meta),
      .obs_imm         (iq_unused_obs_imm),
      .obs_dst_tag     (iq_unused_obs_dstt),
      .obs_dst_gen     (iq_unused_obs_dstg),
      .obs_src1_ready  (iq_unused_obs_s1r),
      .obs_src2_ready  (iq_unused_obs_s2r),
      .obs_src1_val    (iq_unused_obs_s1v),
      .obs_src2_val    (iq_unused_obs_s2v),

      .o_ins_total     (o_ins_total),
      .o_grant_total   (o_grant_total),
      .o_kill_total    (o_kill_total),
      .o_wu_total      (iq_unused_wu_total),
      .o_wu_matched    (iq_unused_wu_matched),
      .o_wu_dup        (iq_unused_wu_dup),
      .o_wu_stale      (iq_unused_wu_stale),
      .o_wu_miss       (iq_wu_miss),
      .o_wu2_matched   (o_wu2_matched)
  );

  assign o_count       = 32'(iq_count);
  assign o_grant_valid = iq_grant_valid;
  assign o_grant_uop   = iq_grant_uop;
  assign o_alu_ctr     = alu_ctr;
  assign o_branch_ctr  = branch_ctr;
  assign o_md_ctr      = md_ctr;
  assign o_refuse_ctr  = refuse_ctr;
  assign o_purge_ctr   = purge_ctr;
  assign o_wu_miss_ctr = iq_wu_miss;

endmodule : mosaic_cluster

`default_nettype wire
