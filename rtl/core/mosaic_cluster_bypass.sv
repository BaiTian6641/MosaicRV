// ============================================================================
// mosaic_cluster_bypass -- the local bypass fast path of one execution cluster
// (work package I-027).
//
// A consumer of a just-produced value does not have to wait for the value to
// become durable. This unit taps the *local* functional unit's result in the
// cycle it is computed, holds it for one cycle in a single-entry bypass slot,
// and lets the cluster's issue candidate take it -- provided the producer's
// identity matches exactly. Everything the fast path declines is served by the
// register file's value-visible wakeup, which is the fallback and is never
// disabled.
//
// ------------------------------------------------------------- where the tap is
//
// The cluster's durable path from a local result to a woken consumer has two
// register boundaries in it:
//
//   cycle N    the local FU computes the result; the cluster's result register
//              takes it at the edge (mosaic_cluster.sv -- `wb_ev_q`);
//   cycle N+1  the WB arbiter's per-producer pending register takes the
//              completion at the edge (mosaic_wb_arbiter.sv -- `pend_v`/
//              `pend_ev`, written only at the edge, so a completion offered in
//              cycle N+1 is published in cycle N+2);
//   cycle N+2  the completion is durable: PRF write, and `wu_valid` raised with
//              the same identity, which the issue queue applies combinationally
//              in the same cycle (mosaic_iq.sv -- `wu_hit1/wu_hit2`).
//
// So a consecutive RAW chain in one cluster runs at **two cycles per link**
// through the register file, with the durable path idle. The local bypass taps
// the FU result *before* the cluster's result register (cycle N), registers it
// once (the registered timing budget, one cycle), and the dependent uop can be
// woken in cycle N+1 -- **one cycle per link**. The measured numbers, rather
// than this argument, are what results/reports/I-027-bypass.md reports.
//
// ----------------------------------------------------------------- the slot
//
// One entry, holding the producer's whole identity and its value:
//
//   {tag, generation}          the physical register version the value is the
//                              value *of*; a match on the tag alone is not a
//                              match (a tag is recycled, and a stale value
//                              installed into a live uop is a wrong result).
//   {rob_index, rob_gen, uop_index}  the macro/uop identity that produced it,
//                              exported so the consumer sees *whose* value it
//                              is and not merely which physical register it
//                              would eventually land in.
//
// The slot is a *best-effort* structure and is allowed to lose an entry: a new
// producer overwrites it, a redirect clears it, and disabling the bypass clears
// it. Nothing is lost by that, because the durable path carries every result
// independently -- the slot only decides whether the consumer may use the value
// one cycle earlier.
//
// -------------------------------------------------- the identity discipline
//
// A value enters the slot only when all of the following hold in the cycle the
// FU computed it:
//
//   * the bypass is armed (`bp_en`);
//   * the producer is present (`p_valid`);
//   * the producing macro is **authorised** (`p_authorised`): the ROB identity
//     is live, so the value is one this machine will retire. An unauthorised
//     producer (a withdrawn grant, a squashed macro) is never forwarded, and is
//     counted (`o_unauth_ctr`);
//   * no redirect is discarding younger work this cycle (`flush`).
//
// The wakeup/value-visibility rule I-026 established is *not* weakened: the
// durable path is untouched, every producer still becomes durable and raises
// exactly one value-visible wakeup, and the bypass publishes no broadcast of
// its own. It only answers, for one identified consumer, "this operand is
// resolved now, from this producer, with this value". The consumer side accepts
// it on an exact (tag, generation) match and only for an operand the queue
// declares outstanding (`c_sN_need`).
//
// ------------------------------------------------------------ the fallback
//
// For every outstanding operand the bypass does not serve, `fb_sN_sel` is
// asserted and the operand is resolved from the durable wakeup `w_*` (matched
// on the same (tag, generation)). A miss therefore *never* stalls: it selects
// the register file, which is what makes the fast path safe to be wrong about.
// `sN_src` says which path resolved the operand, so the decision is observable
// rather than implied.
//
// ------------------------------------------------------- no combinational loop
//
// The match reads only
//
//   * the bypass slot (registers), the candidate's inputs, and the durable
//     broadcast (an input);
//
// and never the producer's result in the same cycle it is computed, and never
// its own outputs. The one register in the path (`slot_v_q` and friends) is
// what breaks the loop the card names: without it, "consumer ready -> FU issue
// -> FU result -> consumer ready" closes in a single cycle. The module is
// therefore unable to form that loop by construction, not by a check: there is
// no combinational path from `p_*` to `sN_rdy`, and no path from `sN_*` back to
// `c_*` or `bp_en`. `MOSAIC_BYPASS_MUTANT_EARLY_TAP` removes the register and
// is a negative control for the *registered timing budget* (it fabricates a
// 0-cycle bypass); it is not claimed as a loop detector, because a loop needs
// the FU and the issue queue as well.
//
// ------------------------------------------------------------------ mutants
//
// -DMOSAIC_BYPASS_MUTANT_<n> injects exactly one defect each, used to prove the
// registered case can fail. The shipping build defines none of them:
//
//   UNAUTH_FORWARD  capture ignores `p_authorised`: a withdrawn grant's value is
//                   forwarded.
//   TAG_ONLY        the match compares the tag and not the generation: a value
//                   is forwarded for a different version of the register.
//   NO_FALLBACK     `sN_rdy` drops the durable term: an operand the bypass
//                   misses is never resolved, so a consumer waits for ever
//                   instead of reading the register file.
//   EARLY_TAP       the match reads the producer's combinational result instead
//                   of the registered slot: a 0-cycle bypass, borrowing the
//                   identity and the timing.
//
// The table with real output is in results/reports/I-027-bypass.md.
// ============================================================================

`default_nettype none
`resetall

/* verilator lint_off UNUSEDPARAM */
// The two generated headers, and no package: the identity widths and the
// geometry come from the same files every other core module reads, and neither
// header is a module that has to be listed as a source of its own.
`include "mosaic_cfg_pkg.svh"
`include "mosaic_id_pkg.svh"
/* verilator lint_on UNUSEDPARAM */

module mosaic_cluster_bypass #(
    // One bypass slot and one producer port: the wiring bound is this module's
    // width, and it is one cluster's, not the fabric's.
    localparam int unsigned XLEN   = mosaic_cfg_pkg::MOSAIC_XLEN,
    localparam int unsigned TAG_W  = mosaic_id_pkg::MOSAIC_ID_W_PRF_TAG,
    localparam int unsigned PGEN_W = mosaic_id_pkg::MOSAIC_ID_W_PRF_GEN,
    localparam int unsigned IDX_W  = mosaic_id_pkg::MOSAIC_ID_W_ROB_INDEX,
    localparam int unsigned RGEN_W = mosaic_id_pkg::MOSAIC_ID_W_ROB_GEN,
    localparam int unsigned UOP_W  = mosaic_id_pkg::MOSAIC_ID_W_UOP_INDEX
) (
    input  logic               clk,
    input  logic               rst,

    // ------------------------------------------------------------- arming
    // The failure/fallback action of the card: with `bp_en` low the bypass
    // serves nothing, every operand takes the durable path, and the slot is
    // cleared so that re-arming it cannot forward a value captured before.
    input  logic               bp_en,

    // --------------------------------------------------- the local producer
    // The cluster's local FU result, in the cycle it is computed (the tap is
    // deliberately *before* the cluster's result register).
    input  logic               p_valid,
    input  logic               p_authorised,
    input  logic [TAG_W-1:0]   p_tag,
    input  logic [PGEN_W-1:0]  p_gen,
    input  logic [IDX_W-1:0]   p_rob_index,
    input  logic [RGEN_W-1:0]  p_rob_gen,
    input  logic [UOP_W-1:0]   p_uop_index,
    input  logic [XLEN-1:0]    p_value,

    // ------------------------------------------------------------- cancel
    // A redirect discards younger work; the slot is cleared at the edge.
    input  logic               flush,

    // ------------------------------------------- the durable wakeup (fallback)
    // The register file's value-visible broadcast: one identity per cycle, from
    // mosaic_wb_arbiter. This is the path a miss falls back to.
    input  logic               w_valid,
    input  logic [TAG_W-1:0]   w_tag,
    input  logic [PGEN_W-1:0]  w_gen,
    input  logic [XLEN-1:0]    w_val,

    // ------------------------------------------------ the candidate consumer
    // The uop the cluster may issue this cycle, and which of its two operands
    // the queue still considers outstanding.
    input  logic               c_valid,
    input  logic [TAG_W-1:0]   c_s1_tag,
    input  logic [PGEN_W-1:0]  c_s1_gen,
    input  logic               c_s1_need,
    input  logic [TAG_W-1:0]   c_s2_tag,
    input  logic [PGEN_W-1:0]  c_s2_gen,
    input  logic               c_s2_need,

    // ------------------------------------------------------- the fast path
    output logic               bp_s1_hit,
    output logic               bp_s2_hit,

    // ------------------------------------------------------- the fallback
    output logic               fb_s1_sel,
    output logic               fb_s2_sel,

    // --------------------------------------------------- resolved operands
    output logic               s1_rdy,
    output logic               s2_rdy,
    output logic [XLEN-1:0]    s1_val,
    output logic [XLEN-1:0]    s2_val,
    // 0 = not resolved by this unit, 1 = local bypass, 2 = durable wakeup
    output logic [1:0]         s1_src,
    output logic [1:0]         s2_src,

    // ------------------------------------------------------- slot identity
    output logic               slot_valid,
    output logic [TAG_W-1:0]   slot_tag,
    output logic [PGEN_W-1:0]  slot_gen,
    output logic [IDX_W-1:0]   slot_rob_index,
    output logic [RGEN_W-1:0]  slot_rob_gen,
    output logic [UOP_W-1:0]   slot_uop_index,

    // ------------------------------------- the early value-visible wakeup (I-090)
    // The source this unit offers a consumer one cycle before the durable
    // broadcast: the registered slot in the shipping build (the producer's own
    // result under the EARLY_TAP control). It is the same selection the match
    // above reads, exported so an issue queue that resolves operands from
    // *wakeups* rather than from per-candidate reads can take the bypass as a
    // second wakeup port instead of fabricating a candidate for the ports on
    // the consumer side above. Those ports remain the module's own contract and
    // are exercised by its registered case.
    output logic               bp_src_valid,
    output logic [TAG_W-1:0]   bp_src_tag,
    output logic [PGEN_W-1:0]  bp_src_gen,
    output logic [XLEN-1:0]    bp_src_value,

    // ---------------------------------------------------------- observation
    output logic               o_slot_captured,
    output logic               o_unauth,
    output logic [31:0]        o_hit_ctr,
    output logic [31:0]        o_miss_ctr,
    output logic [31:0]        o_unauth_ctr,
    output logic [31:0]        o_id_reject_ctr,
    output logic [31:0]        o_flush_ctr
);

  // ------------------------------------------------------------- the slot
  logic              slot_v_q;
  logic [TAG_W-1:0]  slot_tag_q;
  logic [PGEN_W-1:0] slot_gen_q;
  logic [IDX_W-1:0]  slot_idx_q;
  logic [RGEN_W-1:0] slot_rgen_q;
  logic [UOP_W-1:0]  slot_uop_q;
  logic [XLEN-1:0]   slot_val_q;

  // The capture decision: a producer enters the slot at the edge only when the
  // bypass is armed, the producer is present, its macro is authorised and no
  // redirect is in flight.
  logic capture;
`ifdef MOSAIC_BYPASS_MUTANT_UNAUTH_FORWARD
  // MUTANT: the producer's authorisation is not consulted. A withdrawn grant
  // (or a squashed macro) enters the slot and is forwarded to a consumer.
  assign capture = bp_en && p_valid && !flush;
`else
  assign capture = bp_en && p_valid && p_authorised && !flush;
`endif

  logic clear_slot;
  assign clear_slot = flush || !bp_en;

  assign o_slot_captured = capture;
  assign o_unauth        = p_valid && !p_authorised;

  always_ff @(posedge clk) begin
    if (rst) begin
      slot_v_q <= 1'b0;
    end else if (clear_slot) begin
      // A redirect squashes the slot; a disabled bypass empties it. The payload
      // registers are deliberately not cleared (the validity bit is what makes
      // an entry live), which is the rule the rest of this tree follows.
      slot_v_q <= 1'b0;
    end else if (capture) begin
      slot_v_q     <= 1'b1;
      slot_tag_q   <= p_tag;
      slot_gen_q   <= p_gen;
      slot_idx_q   <= p_rob_index;
      slot_rgen_q  <= p_rob_gen;
      slot_uop_q   <= p_uop_index;
      slot_val_q   <= p_value;
    end
  end

  // ------------------------------------------------------- the bypass source
  //
  // The whole match reads this one selection. In the shipping build it is the
  // registered slot; the EARLY_TAP control points it at the producer's
  // combinational result instead.
  logic              bsrc_valid;
  logic [TAG_W-1:0]  bsrc_tag;
  logic [PGEN_W-1:0] bsrc_gen;
  logic [XLEN-1:0]   bsrc_val;

`ifdef MOSAIC_BYPASS_MUTANT_EARLY_TAP
  // MUTANT: a 0-cycle bypass. The producer's own execution cycle is enough for
  // a consumer to be woken, which is the "borrow the timing to reach a
  // single-cycle bypass" defect the card's fail criterion names.
  assign bsrc_valid = p_valid && p_authorised;
  assign bsrc_tag   = p_tag;
  assign bsrc_gen   = p_gen;
  assign bsrc_val   = p_value;
`else
  assign bsrc_valid = slot_v_q;
  assign bsrc_tag   = slot_tag_q;
  assign bsrc_gen   = slot_gen_q;
  assign bsrc_val   = slot_val_q;
`endif

  // ------------------------------------------------------------- the match
  logic m1;
  logic m2;
`ifdef MOSAIC_BYPASS_MUTANT_TAG_ONLY
  // MUTANT: the generation is not compared. A recycled tag's *previous* value
  // is forwarded into a live uop.
  assign m1 = (c_s1_tag == bsrc_tag);
  assign m2 = (c_s2_tag == bsrc_tag);
`else
  assign m1 = (c_s1_tag == bsrc_tag) && (c_s1_gen == bsrc_gen);
  assign m2 = (c_s2_tag == bsrc_tag) && (c_s2_gen == bsrc_gen);
`endif

  assign bp_s1_hit = bp_en && bsrc_valid && c_valid && c_s1_need && m1;
  assign bp_s2_hit = bp_en && bsrc_valid && c_valid && c_s2_need && m2;

  // ---------------------------------------------------------- the fallback
  // The durable broadcast, matched on the same identity. This term is the PRF
  // fallback and is what makes a miss safe.
  logic w1;
  logic w2;
  assign w1 = w_valid && (c_s1_tag == w_tag) && (c_s1_gen == w_gen);
  assign w2 = w_valid && (c_s2_tag == w_tag) && (c_s2_gen == w_gen);

  logic d1;
  logic d2;
  assign d1 = c_valid && c_s1_need && w1;
  assign d2 = c_valid && c_s2_need && w2;

  assign fb_s1_sel = c_valid && c_s1_need && !bp_s1_hit;
  assign fb_s2_sel = c_valid && c_s2_need && !bp_s2_hit;

`ifdef MOSAIC_BYPASS_MUTANT_NO_FALLBACK
  // MUTANT: the durable term is dropped. An operand the bypass misses is never
  // resolved by this unit, so a consumer waits for ever instead of reading the
  // register file.
  assign s1_rdy = bp_s1_hit;
  assign s2_rdy = bp_s2_hit;
`else
  assign s1_rdy = bp_s1_hit || d1;
  assign s2_rdy = bp_s2_hit || d2;
`endif

  assign s1_src = bp_s1_hit ? 2'd1 : (s1_rdy ? 2'd2 : 2'd0);
  assign s2_src = bp_s2_hit ? 2'd1 : (s2_rdy ? 2'd2 : 2'd0);

  assign s1_val = bp_s1_hit ? bsrc_val : w_val;
  assign s2_val = bp_s2_hit ? bsrc_val : w_val;

  // ------------------------------------------------------------- identity
  assign slot_valid     = slot_v_q;
  assign slot_tag       = slot_tag_q;
  assign slot_gen       = slot_gen_q;
  assign slot_rob_index = slot_idx_q;
  assign slot_rob_gen   = slot_rgen_q;
  assign slot_uop_index = slot_uop_q;

  // The early wakeup a consumer takes (I-090): the same source the match reads.
  assign bp_src_valid = bsrc_valid;
  assign bp_src_tag   = bsrc_tag;
  assign bp_src_gen   = bsrc_gen;
  assign bp_src_value = bsrc_val;

  // ---------------------------------------------------------- accountants
  //
  // A tag that matches the slot at another generation is exactly the identity
  // confusion the match must reject, and it is counted separately so "the
  // generation was doing the rejecting" is an observation.
  logic id_reject;
  assign id_reject = bp_en && c_valid && slot_v_q &&
                     ((c_s1_need && (c_s1_tag == slot_tag_q) && (c_s1_gen != slot_gen_q)) ||
                      (c_s2_need && (c_s2_tag == slot_tag_q) && (c_s2_gen != slot_gen_q)));

  logic [1:0] hit_n;
  logic [1:0] miss_n;
  assign hit_n  = {1'b0, bp_s1_hit} + {1'b0, bp_s2_hit};
  assign miss_n = {1'b0, fb_s1_sel} + {1'b0, fb_s2_sel};

  logic [31:0] hit_ctr;
  logic [31:0] miss_ctr;
  logic [31:0] unauth_ctr;
  logic [31:0] id_reject_ctr;
  logic [31:0] flush_ctr;

  always_ff @(posedge clk) begin
    if (rst) begin
      hit_ctr       <= 32'd0;
      miss_ctr      <= 32'd0;
      unauth_ctr    <= 32'd0;
      id_reject_ctr <= 32'd0;
      flush_ctr     <= 32'd0;
    end else begin
      hit_ctr       <= hit_ctr       + {30'd0, hit_n};
      miss_ctr      <= miss_ctr      + {30'd0, miss_n};
      unauth_ctr    <= unauth_ctr    + {31'd0, o_unauth};
      id_reject_ctr <= id_reject_ctr + {31'd0, id_reject};
      flush_ctr     <= flush_ctr     + {31'd0, (flush && slot_v_q)};
    end
  end

  assign o_hit_ctr       = hit_ctr;
  assign o_miss_ctr      = miss_ctr;
  assign o_unauth_ctr    = unauth_ctr;
  assign o_id_reject_ctr = id_reject_ctr;
  assign o_flush_ctr     = flush_ctr;

endmodule : mosaic_cluster_bypass

`default_nettype wire
