// ============================================================================
// mosaic_vec_restart -- the vector partial-trap / `vstart` / fault-only-first
// controller (work package I-057, CASE=rvv.partial_fault_restart).
//
// --------------------------------------------------------------------- why
//
// A vector instruction is one architectural instruction but *not* an atomic
// scalar transaction.  The V spec (pinned tag `3570f998`, `v-spec.adoc`) makes
// the interrupted state architectural:
//
//   * `vstart` "specifies the index of the first element to be executed by a
//     vector instruction"; every vector instruction begins at it and resets it
//     to zero at the end (L543-L560);
//   * on a trap the hardware writes `vstart` = "the element on which the trap
//     was taken", and only the elements before it have taken effect -- the
//     prestart elements "do not update the destination vector register"
//     (L1086-L1100);
//   * a unit-stride fault-only-first load "will only take a trap caused by a
//     synchronous exception on element 0.  If element 0 raises an exception,
//     `vl` is not modified, and the trap is taken.  If an element > 0 raises an
//     exception, the corresponding trap is not taken, and the vector length
//     `vl` is reduced to the index of the element that would have raised an
//     exception." (L1702-L1709);
//   * an interrupt is never absorbed: "implementations should not reduce `vl`
//     and should instead set a `vstart` value" (L1753-L1756).
//
// This module is the controller that turns the packetizer's element-granular
// fault report (`mosaic_vec_lsu`: `o_trap_o`/`o_trap_elem_o`/`o_trap_code_o`,
// and the boundary `o_stopped_o`/`o_stop_elem_o` a precise interrupt needs) into
// those architectural values, holds back the elements after the fault, and
// makes a re-execution from `vstart` perform exactly the remaining elements.
//
// -------------------------------------------------------------- the rules
//
// Stated once, implemented exactly here:
//
//   R1 (partial trap)  A synchronous fault at element k commits the elements
//      strictly before k -- their stores were performed and their loads written
//      back by the packetizer -- latches `vstart = k`, and leaves element k and
//      every later element unperformed.  A whole-macro trap reporting
//      `vstart` = 0 for a fault at k is the card's first fail mode.
//   R2 (no duplicate side effect)  The restart point is the first element *not*
//      yet performed, `vstart` = k, and the re-execution begins there, so a
//      store that already wrote elements 0..k-1 is not written again.  Beginning
//      the restart at 0 would duplicate an irreversible side effect.
//   R3 (fault-only-first)  Only a unit-stride load launched with `exec_fof_i`
//      trims: a fault at element 0 traps with `vl` unmodified; a fault at
//      element k > 0 is absorbed, `vl` becomes k, element k and later are left
//      untouched (so re-executing with the new `vl` re-loads nothing that
//      already succeeded), and no trap is taken.
//   R4 (retire gate)  The macro is complete only after the packetizer reports
//      `done_o` -- every response drained -- not when the last request has been
//      offered.  Retiring on "only the last packet has arrived" is the card's
//      second fail mode.
//
// ------------------------------------------------------ descriptor binding
//
// The descriptor (I-051) already reserves the progress machinery; this module
// binds it, and the binding is exactly this:
//
//   * it drives `elem_done_valid_o`/`elem_done_index_o` with a one-per-cycle
//     walk over the elements that took effect ([run_vstart, commit_end)), so the
//     descriptor's element bitmap records exactly those elements;
//   * it drives `fault_valid_o`/`fault_elem_o`/`fault_code_o` with the earliest
//     fault -- *after* the walk, because the descriptor freezes its bitmap once
//     a fault is recorded (mosaic_vec_desc.sv L719-L736) and would otherwise
//     drop the element completions;
//   * it reads back `desc_bitmap_i`/`desc_prefix_i`: the restart point
//     `o_restart_vstart_o` is the first *clear* bit of the bitmap, and
//     `o_prefix_agree_o` states that the bitmap's first clear bit and the
//     contiguous prefix name the same element.  The restart is therefore driven
//     by the descriptor's own progress, not a second copy of it.
//
// ------------------------------------------------------------- fault codes
//
// The packetizer forwards a 4-bit fault class with each response
// (`mem_rsp_fault_code_i` -> `o_trap_code_o`).  This module consumes it and
// freezes it in the descriptor's `fault_code`.  The encoding, by this module:
//
//   0 none   1 page fault   2 access fault   3 interrupt   4 other
//
// Only `1` and `2` are absorbable by fault-only-first; `3` (interrupt) and `4`
// (anything else) always raise the precise trap.
//
// ------------------------------------------------------------------ mutants
//
// Five `-DMOSAIC_VEC_RESTART_MUTANT_*` defines inject one defect each; the
// shipping build defines none.  Each is proven to fail
// CASE=rvv.partial_fault_restart in results/reports/I-057-vector-restart.md:
//
//   WHOLE_TRAP        a fault at element k is reported with `vstart` = 0, so
//                     the restart cannot resume (the card's first fail mode)
//   REDO_COMMITTED    the restart point is forced to 0, so a re-execution
//                     re-performs an already-committed element -- duplicating a
//                     store side effect (the card's second-to-last sentence)
//   FOF_TRAPS         a fault-only-first load raises the fault instead of
//                     shortening `vl`
//   VSTART_OFF_BY_ONE `vstart` is the element after the faulting one
//   EARLY_RETIRE      the macro is called complete when the last request has
//                     been offered, before the responses drained (the card's
//                     second fail mode)
// ============================================================================

`default_nettype none
`resetall

module mosaic_vec_restart #(
    parameter int unsigned VLEN = 128,
    parameter int unsigned ELEN = 64
) (
    input  logic                  clk_i,
    input  logic                  rst_i,

    // ---- macro launch (a mirror of the packetizer's own launch) -----------
    input  logic                  exec_valid_i,
    input  logic [3:0]            exec_mode_i,
    input  logic                  exec_we_i,
    input  logic                  exec_fof_i,     // fault-only-first load
    input  logic [3:0]            exec_nf_i,      // NFIELDS, for whole-register

    // ---- configuration snapshot (from I-052, the same snapshot the LSU uses)
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [63:0]           cfg_vtype_i,
    /* verilator lint_on UNUSEDSIGNAL */
    input  logic [7:0]            cfg_vl_i,
    input  logic [6:0]            cfg_vstart_i,

    // ---- packetizer observations ------------------------------------------
    input  logic                  lsu_busy_i,
    input  logic                  lsu_done_i,
    input  logic                  lsu_illegal_i,
    input  logic                  lsu_trap_i,
    input  logic [6:0]            lsu_trap_elem_i,
    input  logic [3:0]            lsu_trap_code_i,
    input  logic                  lsu_stopped_i,
    input  logic [6:0]            lsu_stop_elem_i,
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [7:0]            lsu_elems_i,    // items offered so far
    /* verilator lint_on UNUSEDSIGNAL */

    // ---- precise interrupt request, and the boundary stop handshake -------
    input  logic                  intr_i,
    output logic                  stop_o,

    // ---- descriptor binding -----------------------------------------------
    output logic                  elem_done_valid_o,
    output logic [6:0]            elem_done_index_o,
    output logic                  fault_valid_o,
    output logic [6:0]            fault_elem_o,
    output logic [3:0]            fault_code_o,
    input  logic                  desc_valid_i,
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [VLEN-1:0]       desc_bitmap_i,
    /* verilator lint_on UNUSEDSIGNAL */
    input  logic [7:0]            desc_prefix_i,

    // ---- architectural results --------------------------------------------
    output logic                  o_busy_o,
    output logic                  o_resolved_o,
    output logic                  o_illegal_o,
    output logic                  o_trap_o,
    output logic [6:0]            o_vstart_o,
    output logic [3:0]            o_trap_code_o,
    output logic                  o_vl_write_o,
    output logic [7:0]            o_vl_new_o,
    output logic                  o_fof_trim_o,
    output logic                  o_complete_o,
    output logic                  o_retire_ok_o,
    output logic                  o_restart_ready_o,
    output logic [6:0]            o_restart_vstart_o,
    output logic [7:0]            o_elems_committed_o,
    output logic                  o_prefix_agree_o
);

  // ------------------------------------------------------------------ modes
  /* verilator lint_off UNUSEDPARAM */
  localparam logic [3:0] LM_UNIT  = 4'd0;
  localparam logic [3:0] LM_WHOLE = 4'd6;
  localparam logic [3:0] LM_MASK  = 4'd7;
  /* verilator lint_on UNUSEDPARAM */

  // ------------------------------------------------------------ fault codes
  /* verilator lint_off UNUSEDPARAM */
  localparam logic [3:0] RC_NONE   = 4'd0;
  localparam logic [3:0] RC_PAGE   = 4'd1;
  localparam logic [3:0] RC_ACCESS = 4'd2;
  localparam logic [3:0] RC_INTR   = 4'd3;
  localparam logic [3:0] RC_OTHER  = 4'd4;
  /* verilator lint_on UNUSEDPARAM */

  // --------------------------------------------------------------- states
  localparam logic [1:0] S_IDLE = 2'd0;
  localparam logic [1:0] S_RUN  = 2'd1;
  localparam logic [1:0] S_POST = 2'd2;

  // A mutant build makes some of these registers unused; the shipping build uses
  // every one, and the mutants must still lint clean so their evidence is a
  // clean build.
  /* verilator lint_off UNUSEDSIGNAL */
  logic [1:0]  state_q;

  // the command latched at launch
  logic [6:0]  run_start_q;
  logic [7:0]  run_vl_q;
  logic [3:0]  run_mode_q;
  logic [3:0]  run_nf_q;
  logic        run_we_q;
  logic        run_fof_q;

  // the fault latched while the packetizer drains
  logic        pf_q;
  logic [6:0]  pf_elem_q;
  logic [3:0]  pf_code_q;

  // the interrupt stop handshake
  logic        stopping_q;

  // the post-pass walk over the committed elements
  logic        fault_pulsed_q;
  logic [7:0]  commit_end_q;
  logic [7:0]  walk_q;
  logic [7:0]  committed_q;

  // the registered architectural results
  logic        resolved_q;
  logic        trap_q;
  logic [6:0]  vstart_q;
  logic [3:0]  tcode_q;
  logic        vlw_q;
  logic [7:0]  vl_new_q;
  logic        fof_q;
  logic        comp_q;
  logic        ret_ok_q;
  logic        rready_q;
  logic        illegal_q;
  /* verilator lint_on UNUSEDSIGNAL */

  // ---------------------------------------------------- derived: element end
  // the exclusive end of this run's elements, one rule per mode, mirroring the
  // packetizer's own `elem_end`
  int unsigned sew_l;
  int unsigned be;
  logic [7:0]  elem_end_c;
  always_comb begin
    // cfg_vtype_i[5:3] is the RVV 1.0 vsew field (0..3); log2(SEW) = vsew + 3.
    sew_l = int'(cfg_vtype_i[5:3]) + 3;
    be    = ((sew_l >= 3) && (sew_l <= 6) && ((1 << sew_l) <= int'(ELEN)))
                ? (1 << sew_l) : 1;
    case (run_mode_q)
      LM_WHOLE: elem_end_c = 8'(int'(run_nf_q) * (int'(VLEN) / int'(be)));
      LM_MASK:  elem_end_c = 8'((int'(run_vl_q) + 7) >> 3);
      default:  elem_end_c = run_vl_q;
    endcase
  end

  // ---------------------------------------------------- derived: FOF absorb
  // R3: a unit-stride fault-only-first *load*, an absorbable class, and an
  // element index strictly above zero (element 0 raises the trap with `vl`
  // unmodified).
  logic fof_absorb_c;
`ifdef MOSAIC_VEC_RESTART_MUTANT_FOF_TRAPS
  // NEGATIVE CONTROL: the fault-only-first form raises the fault instead of
  // shortening `vl`.
  assign fof_absorb_c = 1'b0;
`else
  assign fof_absorb_c = run_fof_q && !run_we_q && (run_mode_q == LM_UNIT) &&
                        ((pf_code_q == RC_PAGE) || (pf_code_q == RC_ACCESS)) &&
                        (pf_elem_q != 7'd0);
`endif

  // --------------------------------------------------- derived: bitmap view
  // the restart point is the first element not yet performed: the first clear
  // bit of the descriptor's element bitmap.  Computing it from the bitmap (and
  // cross-checking the descriptor's contiguous prefix) is the binding between
  // this controller and I-051's progress machinery.
  logic [7:0] first_clear_c;
  always_comb begin
    first_clear_c = 8'(VLEN);
    for (int unsigned i = 0; i < VLEN; i++) begin
      if ((first_clear_c == 8'(VLEN)) && !desc_bitmap_i[i]) first_clear_c = 8'(i);
    end
  end

  // ------------------------------------------------------------ the FSM
  always_ff @(posedge clk_i) begin
    if (rst_i) begin
      state_q        <= S_IDLE;
      run_start_q    <= 7'd0;
      run_vl_q       <= 8'd0;
      run_mode_q     <= 4'd0;
      run_nf_q       <= 4'd1;
      run_we_q       <= 1'b0;
      run_fof_q      <= 1'b0;
      pf_q           <= 1'b0;
      pf_elem_q      <= 7'd0;
      pf_code_q      <= RC_NONE;
      stopping_q     <= 1'b0;
      fault_pulsed_q <= 1'b0;
      commit_end_q   <= 8'd0;
      walk_q         <= 8'd0;
      committed_q    <= 8'd0;
      resolved_q     <= 1'b0;
      trap_q         <= 1'b0;
      vstart_q       <= 7'd0;
      tcode_q        <= RC_NONE;
      vlw_q          <= 1'b0;
      vl_new_q       <= 8'd0;
      fof_q          <= 1'b0;
      comp_q         <= 1'b0;
      ret_ok_q       <= 1'b0;
      rready_q       <= 1'b0;
      illegal_q      <= 1'b0;
    end else begin
      if (exec_valid_i && (state_q == S_IDLE)) begin
        run_start_q    <= cfg_vstart_i;
        run_vl_q       <= cfg_vl_i;
        run_mode_q     <= exec_mode_i;
        run_nf_q       <= (exec_nf_i == 4'd0) ? 4'd1 : exec_nf_i;
        run_we_q       <= exec_we_i;
        run_fof_q      <= exec_fof_i;
        pf_q           <= 1'b0;
        stopping_q     <= 1'b0;
        fault_pulsed_q <= 1'b0;
        resolved_q     <= 1'b0;
        illegal_q      <= 1'b0;
        rready_q       <= 1'b0;
        state_q        <= S_RUN;
      end else begin
        case (state_q)
          // ------------------------------------------------------- running
          S_RUN: begin
            if (intr_i && !stopping_q && !lsu_done_i) stopping_q <= 1'b1;
            if (lsu_stopped_i && !pf_q) begin
              pf_q      <= 1'b1;
              pf_elem_q <= lsu_stop_elem_i;
              pf_code_q <= RC_INTR;
            end else if (lsu_trap_i && !pf_q) begin
              pf_q      <= 1'b1;
              pf_elem_q <= lsu_trap_elem_i;
              pf_code_q <= lsu_trap_code_i;
            end
            if (lsu_done_i) begin
              resolved_q <= 1'b1;
              illegal_q  <= lsu_illegal_i;
              if (pf_q && !fof_absorb_c) begin
                // R1: the precise partial trap.  `vstart` = the faulting index.
                trap_q       <= 1'b1;
                vstart_q     <= pf_elem_q;
                tcode_q      <= pf_code_q;
                vlw_q        <= 1'b0;
                vl_new_q     <= 8'd0;
                fof_q        <= 1'b0;
                comp_q       <= 1'b0;
                ret_ok_q     <= 1'b0;
                commit_end_q <= {1'b0, pf_elem_q};
                committed_q  <= 8'({1'b0, pf_elem_q}) - 8'({1'b0, run_start_q});
              end else if (pf_q) begin
                // R3: the fault-only-first trim.  `vl` = the faulting index,
                // no trap, element k and later untouched.
                trap_q       <= 1'b0;
                vstart_q     <= pf_elem_q;
                tcode_q      <= pf_code_q;
                vlw_q        <= 1'b1;
                vl_new_q     <= {1'b0, pf_elem_q};
                fof_q        <= 1'b1;
                comp_q       <= 1'b1;
                ret_ok_q     <= 1'b1;
                commit_end_q <= {1'b0, pf_elem_q};
                committed_q  <= 8'({1'b0, pf_elem_q}) - 8'({1'b0, run_start_q});
              end else begin
                // clean completion: every body element took effect
                trap_q       <= 1'b0;
                vstart_q     <= 7'd0;
                tcode_q      <= RC_NONE;
                vlw_q        <= 1'b0;
                vl_new_q     <= 8'd0;
                fof_q        <= 1'b0;
                comp_q       <= 1'b1;
                ret_ok_q     <= 1'b1;
                commit_end_q <= elem_end_c;
                committed_q  <= elem_end_c - 8'({1'b0, run_start_q});
              end
              walk_q <= 8'({1'b0, run_start_q});
              state_q <= S_POST;
            end
          end

          // ------------------------------------- walk the committed elements
          S_POST: begin
            if (walk_q < commit_end_q) begin
              walk_q <= walk_q + 8'd1;
            end else if (trap_q && !fault_pulsed_q) begin
              fault_pulsed_q <= 1'b1;
            end else begin
              rready_q <= trap_q;
              state_q  <= S_IDLE;
            end
          end

          default: state_q <= S_IDLE;
        endcase
      end
    end
  end

  // ------------------------------------------------------------ walker drive
  assign elem_done_valid_o = (state_q == S_POST) && (walk_q < commit_end_q);
  assign elem_done_index_o = walk_q[6:0];

  // the fault record is written only after the walk, and only for a trap
  assign fault_valid_o = (state_q == S_POST) && trap_q && (walk_q >= commit_end_q) &&
                         !fault_pulsed_q;
  assign fault_elem_o  = pf_elem_q[6:0];
  assign fault_code_o  = pf_code_q;

  // the boundary stop the precise interrupt needs
  assign stop_o = (state_q == S_RUN) && stopping_q && !pf_q;

  // ------------------------------------------------------------ architecture
  assign o_busy_o         = (state_q != S_IDLE) || lsu_busy_i;
  assign o_resolved_o     = resolved_q;
  assign o_trap_o         = trap_q;
`ifdef MOSAIC_VEC_RESTART_MUTANT_WHOLE_TRAP
  // NEGATIVE CONTROL: a fault at element k is reported with `vstart` = 0.
  assign o_vstart_o       = 7'd0;
`elsif MOSAIC_VEC_RESTART_MUTANT_VSTART_OFF_BY_ONE
  // NEGATIVE CONTROL: `vstart` is the element after the faulting one.
  assign o_vstart_o       = vstart_q + 7'd1;
`else
  assign o_vstart_o       = vstart_q;
`endif
  assign o_trap_code_o    = tcode_q;
  assign o_vl_write_o     = vlw_q;
  assign o_vl_new_o       = vl_new_q;
  assign o_fof_trim_o     = fof_q;
  assign o_elems_committed_o = committed_q;
  assign o_restart_ready_o   = rready_q && desc_valid_i;
`ifdef MOSAIC_VEC_RESTART_MUTANT_REDO_COMMITTED
  // NEGATIVE CONTROL: the restart point is 0, so a re-execution re-performs the
  // elements that already took effect -- duplicating a store side effect.
  assign o_restart_vstart_o = 7'd0;
`else
  assign o_restart_vstart_o = first_clear_c[6:0];
`endif
  assign o_prefix_agree_o = (desc_prefix_i == first_clear_c);

`ifdef MOSAIC_VEC_RESTART_MUTANT_EARLY_RETIRE
  // NEGATIVE CONTROL: the macro is called complete when the last request has
  // been offered, before the responses drained.
  assign o_complete_o  = (state_q == S_RUN) && (int'(lsu_elems_i) >= int'(elem_end_c));
  assign o_retire_ok_o = o_complete_o;
`else
  assign o_complete_o  = comp_q;
  assign o_retire_ok_o = ret_ok_q;
`endif

  // `o_illegal_o` is part of the packetizer's outcome; expose it so the case can
  // tell a refused command from a completed one.
  assign o_illegal_o = illegal_q;

endmodule : mosaic_vec_restart

`resetall
`default_nettype wire
