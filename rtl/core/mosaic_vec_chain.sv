// ============================================================================
// mosaic_vec_chain -- the vector chaining network (work package I-058,
// CASE=rvv.chaining_hazards).
//
// --------------------------------------------------------------------- why
//
// A vector macro is one architectural instruction, but its elements are *not*
// one unit of execution.  A dependent macro can begin element i as soon as its
// producer's element i exists, instead of waiting for the whole producing macro
// to finish -- that is what the V spec's chaining licence permits, and it is
// what this network implements.  The network is deliberately element-granular
// because two hazards are invisible at macro granularity:
//
//   * a *macro-level ready bit* (one element written makes the whole producing
//     macro "ready") lets a consumer read an element its producer has not
//     written yet.  It looks correct for a whole macro and is wrong for one
//     element; the card names it the first fail mode.
//   * a *cancelled* macro's element packet must not be accepted by the new
//     descriptor that reuses the slot.  A packet therefore carries the identity
//     of the descriptor generation that produced it, and a mismatched packet is
//     refused -- the same identity discipline the store queue, the TLB and the
//     MSHR all needed; the card names the leak the second fail mode.
//
// ------------------------------------------------------------------ the rules
//
// Stated once, implemented exactly here:
//
//   R1 (element readiness / chaining)  A consumer C of the produced group may
//      read element i when the producer's element i has been written and
//      buffered (`rdq[i]`).  With `chain_en_i = 0` -- the safe no-chaining
//      control -- C waits for the producer's whole macro (`done_q`) and reads
//      the same element values, only later.  Readiness never depends on the
//      producer retiring or on the descriptor being released: execution is
//      decoupled from commit.
//   R2 (element identity)  A producer element packet is accepted only if it
//      carries the group's current producer generation, its index is inside
//      `vl`, the macro has not faulted, and the element is not already written.
//      A packet from a cancelled or superseded generation is refused and enters
//      neither the forwarding scoreboard nor the descriptor's progress.
//   R3 (source lifetime / WAR)  A consumer that holds the group as a source
//      registers a per-element hold over [0, vl).  A hold on element i is
//      discharged only by that consumer reading element i, or by that consumer
//      being cancelled.  An overwrite of group G at element i (`war_grant_o`)
//      and the release of G as a whole (`src_release_ok_o`) are granted only
//      when no hold remains on that element / on any element -- a reader that
//      finished must not discharge another reader's hold.
//   R4 (element order)  Reading element i returns the producer's element i.
//      The forwarding buffer is indexed by the element index and a grant for
//      index i never reads a different element's slot: element order within a
//      macro is the contract.
//
// ------------------------------------------------------ the scoreboard shape
//
// The element scoreboard here is *parallel* to the descriptor's element bitmap
// (I-051) rather than an extension of it, and the two facts are different:
//
//   * the descriptor's bitmap is architectural progress -- it is bound to the
//     restart controller (I-057) and to the committed prefix a trap resumes
//     from, and it must never record a speculative or wrong-path packet;
//   * the chain's readiness also has to carry the *forwarded element data* and
//     the *generation* the packet came from, which the descriptor does not hold,
//     and it must advance on an element *write* rather than on architectural
//     commit.
//
// The two are kept equal by construction rather than by luck: when the network
// drives the descriptor (`chain_bind_i` in the wrapper), the descriptor's
// `elem_done_*` is exactly the network's accepted-packet pulse, so only an
// accepted, in-`vl`, non-faulted packet advances either.
//
// ------------------------------------------------------------------ mutants
//
// Four `-DMOSAIC_VEC_CHAIN_MUTANT_*` defines inject one defect each; the
// shipping build defines none.  Each is proven to fail CASE=rvv.chaining_hazards
// in results/reports/I-058-vector-chaining.md:
//
//   MACRO_READY    a macro-level ready bit: one element written makes the whole
//                  macro look ready, so a consumer reads an element its producer
//                  has not written (the card's first fail mode)
//   STALE_PACKET   a packet from a cancelled generation is accepted by the new
//                  descriptor (the card's second fail mode)
//   WAR_RELEASE    a WAR release fires because *some* reader finished, ignoring
//                  a reader whose element read is still outstanding
//   REORDER        the forwarding path returns another element's data, breaking
//                  the element-order contract
// ============================================================================

`default_nettype none
`resetall

module mosaic_vec_chain #(
    parameter int unsigned VLEN    = 128,
    parameter int unsigned ELEN    = 64,
    parameter int unsigned GEN_W   = 7,
    parameter int unsigned GROUP_W = 5
) (
    input  logic                     clk_i,
    input  logic                     rst_i,

    // ---- chaining enable: 0 is the safe no-chaining control -----------------
    input  logic                     chain_en_i,

    // ---- producer port (the macro whose elements consumers chain from) ------
    input  logic                     p_alloc_valid_i,
    output logic                     p_alloc_ready_o,
    input  logic [GEN_W-1:0]         p_gen_i,
    input  logic [GROUP_W-1:0]       p_vd_i,
    input  logic [7:0]               p_vl_i,

    input  logic                     p_wr_valid_i,
    input  logic [6:0]               p_wr_index_i,
    input  logic [ELEN-1:0]          p_wr_data_i,
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [GEN_W-1:0]         p_wr_gen_i,
    /* verilator lint_on UNUSEDSIGNAL */
    output logic                     p_wr_accept_o,

    input  logic                     p_fault_valid_i,
    input  logic [6:0]               p_fault_elem_i,
    input  logic                     p_cancel_i,
    input  logic                     p_done_i,

    // ---- validated element completion handed to the descriptor --------------
    output logic                     o_accept_valid_o,
    output logic [6:0]               o_accept_index_o,

    // ---- consumer 0 (a younger macro reading the produced group) -----------
    input  logic                     c0_alloc_valid_i,
    output logic                     c0_alloc_ready_o,
    input  logic [GEN_W-1:0]         c0_gen_i,
    input  logic [GROUP_W-1:0]       c0_vs_i,
    input  logic [7:0]               c0_vl_i,
    input  logic                     c0_req_valid_i,
    input  logic [6:0]               c0_req_index_i,
    output logic                     c0_req_accept_o,
    output logic                     c0_rdy_o,
    output logic [ELEN-1:0]          c0_data_o,
    input  logic                     c0_finish_i,

    // ---- consumer 1 ---------------------------------------------------------
    input  logic                     c1_alloc_valid_i,
    output logic                     c1_alloc_ready_o,
    input  logic [GEN_W-1:0]         c1_gen_i,
    input  logic [GROUP_W-1:0]       c1_vs_i,
    input  logic [7:0]               c1_vl_i,
    input  logic                     c1_req_valid_i,
    input  logic [6:0]               c1_req_index_i,
    output logic                     c1_req_accept_o,
    output logic                     c1_rdy_o,
    output logic [ELEN-1:0]          c1_data_o,
    input  logic                     c1_finish_i,

    // ---- WAR overwrite arbiter (a successor writer of the group) -----------
    input  logic                     war_valid_i,
    input  logic [GROUP_W-1:0]       war_vd_i,
    input  logic [6:0]               war_elem_i,
    input  logic [ELEN-1:0]          war_data_i,
    output logic                     war_grant_o,
    output logic                     src_release_ok_o,

    // ---- read-back for the case ---------------------------------------------
    output logic                     o_valid_o,
    output logic [GEN_W-1:0]         o_gen_o,
    output logic [GROUP_W-1:0]       o_vd_o,
    output logic                     o_done_o,
    output logic                     o_fault_o,
    output logic [6:0]               o_fault_elem_o,
    output logic [VLEN-1:0]          o_ready_o,

    // ---- counters (the on/off comparison and coverage) ----------------------
    output logic [15:0]              o_pkt_accept_ctr_o,
    output logic [15:0]              o_pkt_refuse_ctr_o,
    output logic [15:0]              o_fwd_ctr_o,
    output logic [15:0]              o_stall_ctr_o
);

  // ---------------------------------------------------------------- storage
  logic                valid_q;
  logic [GEN_W-1:0]    gen_q;
  logic [GROUP_W-1:0]  vd_q;
  logic [7:0]          vl_q;
  logic                done_q;
  logic                fault_q;
  logic [6:0]          fault_elem_q;
  logic [VLEN-1:0]     rdq_q;
  logic [ELEN-1:0]     buf_q [VLEN];

  logic                c_valid_q [2];
  logic [GEN_W-1:0]    c_gen_q   [2];
  logic [GROUP_W-1:0]  c_vs_q    [2];
  logic [7:0]          c_vl_q    [2];
  logic [VLEN-1:0]     c_read_q  [2];

  /* verilator lint_off UNUSEDSIGNAL */
  logic [15:0]         accept_ctr_q;
  logic [15:0]         refuse_ctr_q;
  logic [15:0]         fwd_ctr_q;
  logic [15:0]         stall_ctr_q;
  /* verilator lint_on UNUSEDSIGNAL */

  // ------------------------------------------------- producer packet identity
  // R2.  A packet names the descriptor generation that produced it; a packet
  // from a cancelled or superseded generation must not be accepted.
  logic gen_match_c;
`ifdef MOSAIC_VEC_CHAIN_MUTANT_STALE_PACKET
  // NEGATIVE CONTROL: the generation check is dropped, so a cancelled macro's
  // packet is accepted by the new descriptor (the card's second fail mode).
  assign gen_match_c = 1'b1;
`else
  assign gen_match_c = (p_wr_gen_i == gen_q);
`endif

  logic idx_in_vl_c;
  logic pkt_ok_c;
  always_comb begin
    idx_in_vl_c = ({1'b0, p_wr_index_i} < vl_q);
    pkt_ok_c    = p_wr_valid_i && valid_q && !fault_q && gen_match_c &&
                  idx_in_vl_c && !rdq_q[p_wr_index_i];
  end

  assign p_wr_accept_o    = pkt_ok_c;
  assign o_accept_valid_o = pkt_ok_c;
  assign o_accept_index_o = p_wr_index_i;

  // ---------------------------------------------------- consumer readiness
  logic c0_rdy_c;
  logic c1_rdy_c;
  logic c0_gen_ok_c;
  logic c1_gen_ok_c;
  logic c0_idx_ok_c;
  logic c1_idx_ok_c;
  logic c0_acc_c;
  logic c1_acc_c;

  /* verilator lint_off UNUSEDSIGNAL */
  // R1: element-granular readiness.  `chain_en_i = 0` is the safe control: the
  // consumer waits for the producer's whole macro instead of one element.
  logic c0_elem_rdy_c;
  logic c1_elem_rdy_c;
  assign c0_elem_rdy_c = rdq_q[c0_req_index_i];
  assign c1_elem_rdy_c = rdq_q[c1_req_index_i];
`ifdef MOSAIC_VEC_CHAIN_MUTANT_MACRO_READY
  // NEGATIVE CONTROL: a macro-level ready bit -- one element written makes the
  // whole producing macro look ready, so a consumer reads an element its
  // producer has not written (the card's first fail mode).
  assign c0_rdy_c = chain_en_i ? (|rdq_q) : done_q;
  assign c1_rdy_c = chain_en_i ? (|rdq_q) : done_q;
`else
  assign c0_rdy_c = chain_en_i ? c0_elem_rdy_c : done_q;
  assign c1_rdy_c = chain_en_i ? c1_elem_rdy_c : done_q;
`endif
  /* verilator lint_on UNUSEDSIGNAL */

  assign c0_gen_ok_c = (c_gen_q[0] == gen_q);
  assign c1_gen_ok_c = (c_gen_q[1] == gen_q);
  assign c0_idx_ok_c = ({1'b0, c0_req_index_i} < c_vl_q[0]);
  assign c1_idx_ok_c = ({1'b0, c1_req_index_i} < c_vl_q[1]);

  assign c0_acc_c = c0_req_valid_i && c_valid_q[0] && valid_q && c0_gen_ok_c &&
                    c0_idx_ok_c && c0_rdy_c;
  assign c1_acc_c = c1_req_valid_i && c_valid_q[1] && valid_q && c1_gen_ok_c &&
                    c1_idx_ok_c && c1_rdy_c;

  assign c0_req_accept_o = c0_acc_c;
  assign c1_req_accept_o = c1_acc_c;
  assign c0_rdy_o        = c0_rdy_c;
  assign c1_rdy_o        = c1_rdy_c;

  assign c0_alloc_ready_o = !c_valid_q[0];
  assign c1_alloc_ready_o = !c_valid_q[1];

  // R4: the forwarded value is the requested element's own slot.
`ifdef MOSAIC_VEC_CHAIN_MUTANT_REORDER
  // NEGATIVE CONTROL: the forwarding path returns the neighbouring element, so
  // the element-order contract inside one macro is broken.
  assign c0_data_o = buf_q[c0_req_index_i + 7'd1];
  assign c1_data_o = buf_q[c1_req_index_i + 7'd1];
`else
  assign c0_data_o = buf_q[c0_req_index_i];
  assign c1_data_o = buf_q[c1_req_index_i];
`endif

  // ----------------------------------------------------- source lifetime
  // R3.  A hold on element i exists while a registered consumer still needs
  // element i of the group it holds as a source and has not read it.  The
  // element-granular overwrite grant and the whole-group release are the
  // element-wise and group-wise views of the same fact.
  logic c0_hold_war_c;
  logic c1_hold_war_c;
  logic c0_hold_any_c;
  logic c1_hold_any_c;
  /* verilator lint_off UNUSEDSIGNAL */
  logic hold_war_c;
  /* verilator lint_on UNUSEDSIGNAL */

  assign c0_hold_war_c = c_valid_q[0] && (c_vs_q[0] == war_vd_i) &&
                         ({1'b0, war_elem_i} < c_vl_q[0]) && !c_read_q[0][war_elem_i];
  assign c1_hold_war_c = c_valid_q[1] && (c_vs_q[1] == war_vd_i) &&
                         ({1'b0, war_elem_i} < c_vl_q[1]) && !c_read_q[1][war_elem_i];
  assign hold_war_c    = c0_hold_war_c || c1_hold_war_c;

  always_comb begin
    c0_hold_any_c = 1'b0;
    c1_hold_any_c = 1'b0;
    for (int unsigned i = 0; i < VLEN; i++) begin
      if (c_valid_q[0] && (c_vs_q[0] == vd_q) && (8'(i) < c_vl_q[0]) &&
          !c_read_q[0][i]) c0_hold_any_c = 1'b1;
      if (c_valid_q[1] && (c_vs_q[1] == vd_q) && (8'(i) < c_vl_q[1]) &&
          !c_read_q[1][i]) c1_hold_any_c = 1'b1;
    end
  end

`ifdef MOSAIC_VEC_CHAIN_MUTANT_WAR_RELEASE
  // NEGATIVE CONTROL: the release fires when some other reader is free,
  // ignoring the reader whose element read is still outstanding.
  assign war_grant_o      = war_valid_i && valid_q && !(c0_hold_war_c && c1_hold_war_c);
  assign src_release_ok_o = valid_q && !(c0_hold_any_c && c1_hold_any_c);
`else
  assign war_grant_o      = war_valid_i && valid_q && !hold_war_c;
  assign src_release_ok_o = valid_q && !c0_hold_any_c && !c1_hold_any_c;
`endif

  // ------------------------------------------------------------------ state
  logic c0_alloc_go_c;
  logic c1_alloc_go_c;
  logic c0_fin_go_c;
  logic c1_fin_go_c;
  logic war_go_c;
  logic stall_c;

  assign c0_alloc_go_c = c0_alloc_valid_i && c0_alloc_ready_o;
  assign c1_alloc_go_c = c1_alloc_valid_i && c1_alloc_ready_o;
  assign c0_fin_go_c   = c0_finish_i;
  assign c1_fin_go_c   = c1_finish_i;
  assign war_go_c      = war_valid_i && war_grant_o;
  assign stall_c       = (c0_req_valid_i && !c0_acc_c) || (c1_req_valid_i && !c1_acc_c);

  always_ff @(posedge clk_i) begin
    if (rst_i) begin
      valid_q      <= 1'b0;
      gen_q        <= {GEN_W{1'b0}};
      vd_q         <= {GROUP_W{1'b0}};
      vl_q         <= 8'd0;
      done_q       <= 1'b0;
      fault_q      <= 1'b0;
      fault_elem_q <= 7'd0;
      rdq_q        <= {VLEN{1'b0}};
      for (int unsigned i = 0; i < VLEN; i++) buf_q[i] <= {ELEN{1'b0}};
      for (int unsigned c = 0; c < 2; c++) begin
        c_valid_q[c] <= 1'b0;
        c_gen_q[c]   <= {GEN_W{1'b0}};
        c_vs_q[c]    <= {GROUP_W{1'b0}};
        c_vl_q[c]    <= 8'd0;
        c_read_q[c]  <= {VLEN{1'b0}};
      end
      accept_ctr_q <= 16'd0;
      refuse_ctr_q <= 16'd0;
      fwd_ctr_q    <= 16'd0;
      stall_ctr_q  <= 16'd0;
    end else begin
      // ---- producer ------------------------------------------------------
      if (p_cancel_i) begin
        // A cancelled macro's readiness must not be consumable: the generation
        // stops being the group's and the scoreboard empties.
        valid_q <= 1'b0;
        done_q  <= 1'b0;
        fault_q <= 1'b0;
        rdq_q   <= {VLEN{1'b0}};
      end
      if (p_done_i && valid_q) done_q <= 1'b1;
      if (p_fault_valid_i && valid_q && !fault_q) begin
        fault_q      <= 1'b1;
        fault_elem_q <= p_fault_elem_i;
      end

      // An accepted packet advances the element scoreboard and the buffer.
      if (pkt_ok_c) begin
        rdq_q[p_wr_index_i] <= 1'b1;
        buf_q[p_wr_index_i] <= p_wr_data_i;
      end

      // A granted overwrite lands the successor's value in the element slot.
      if (war_go_c) begin
        rdq_q[war_elem_i] <= 1'b1;
        buf_q[war_elem_i] <= war_data_i;
      end

      // ---- consumers -----------------------------------------------------
      if (c0_alloc_go_c) begin
        c_valid_q[0] <= 1'b1;
        c_gen_q[0]   <= c0_gen_i;
        c_vs_q[0]    <= c0_vs_i;
        c_vl_q[0]    <= c0_vl_i;
        c_read_q[0]  <= {VLEN{1'b0}};
      end
      if (c1_alloc_go_c) begin
        c_valid_q[1] <= 1'b1;
        c_gen_q[1]   <= c1_gen_i;
        c_vs_q[1]    <= c1_vs_i;
        c_vl_q[1]    <= c1_vl_i;
        c_read_q[1]  <= {VLEN{1'b0}};
      end
      if (c0_fin_go_c) c_valid_q[0] <= 1'b0;
      if (c1_fin_go_c) c_valid_q[1] <= 1'b0;
      // A read discharges exactly that consumer's hold on that element.
      if (c0_acc_c) c_read_q[0][c0_req_index_i] <= 1'b1;
      if (c1_acc_c) c_read_q[1][c1_req_index_i] <= 1'b1;

      // ---- allocate last, so a same-cycle cancel-and-allocate installs the
      //      new macro (the ordering is stated, not incidental) --------------
      if (p_alloc_valid_i && p_alloc_ready_o) begin
        valid_q      <= 1'b1;
        gen_q        <= p_gen_i;
        vd_q         <= p_vd_i;
        vl_q         <= p_vl_i;
        done_q       <= 1'b0;
        fault_q      <= 1'b0;
        fault_elem_q <= 7'd0;
        rdq_q        <= {VLEN{1'b0}};
      end

      // ---- counters ------------------------------------------------------
      if (pkt_ok_c) accept_ctr_q <= accept_ctr_q + 16'd1;
      if (p_wr_valid_i && !pkt_ok_c) refuse_ctr_q <= refuse_ctr_q + 16'd1;
      if ((c0_acc_c || c1_acc_c) && chain_en_i && !done_q) begin
        fwd_ctr_q <= fwd_ctr_q + 16'd1;
      end
      if (stall_c) stall_ctr_q <= stall_ctr_q + 16'd1;
    end
  end

  // ------------------------------------------------------------- read-back
  assign p_alloc_ready_o    = !valid_q || p_cancel_i;
  assign o_valid_o          = valid_q;
  assign o_gen_o            = gen_q;
  assign o_vd_o             = vd_q;
  assign o_done_o           = done_q;
  assign o_fault_o          = fault_q;
  assign o_fault_elem_o     = fault_elem_q;
  assign o_ready_o          = rdq_q;
  assign o_pkt_accept_ctr_o = accept_ctr_q;
  assign o_pkt_refuse_ctr_o = refuse_ctr_q;
  assign o_fwd_ctr_o        = fwd_ctr_q;
  assign o_stall_ctr_o      = stall_ctr_q;

endmodule : mosaic_vec_chain

`resetall
`default_nettype wire
