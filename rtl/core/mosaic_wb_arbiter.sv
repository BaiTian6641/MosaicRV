// ============================================================================
// mosaic_wb_arbiter -- the completion path (work package I-026).
//
// Three producers deliver a `mosaic_uop_pkg::wb_event_t`: the two clusters, and
// the shared MUL/DIV unit (whose result arrives out of band with its identity).
// This module is the single place where a completion becomes architectural:
// it writes the value into the physical register file, tells rename that the
// producer for that (tag, generation) has written, tells the ROB the uop is
// done, publishes the *value-visible* wakeup the issue queues consume, and keeps
// the durable value where the retire path can read it back.
//
// ------------------------------------------------------- value-visible wakeup
//
// A wakeup is published **only in the cycle the value is written**, and only
// when the write was actually accepted:
//
//   * rename is the authority on "this (tag, generation) is the current owner
//     of this tag and has not written yet". A completion it refuses is stale (a
//     previous owner) or duplicate (a second producer for a generation that is
//     already written), and neither may write a physical register.
//   * The PRF write, the rename offer and the wakeup are driven from the same
//     decision in the same cycle, so the value the IQ captures is the value the
//     PRF is storing at that edge. There is no window in which a consumer can
//     be woken for a value the register file has not yet taken.
//
// The negative control `MOSAIC_WB_MUTANT_EARLY_WAKE` wakes on a completion the
// register file did not take, which is the card's "FU done 提前 wakeup, 而值在
// 后续争用中失踪" stated as a defect.
//
// ------------------------------------------------------- one producer / write
//
// `mosaic_rename` has a single writeback port, so exactly one completion is
// published per cycle even though the PRF has one write port per bank. Two
// completions offered in one cycle are both captured (nothing is lost) and one
// is published; if both target the same bank the second is a PRF write-port
// collision and is *delayed*, never dropped -- `o_collision_ctr` counts it and
// `o_wr_ctr` proves both eventually wrote. Higher completion throughput needs a
// second rename writeback port (I-025/I-027 territory), not a second copy of
// this rule.
//
// A completion that carries no destination (a store, a fence, a branch without
// a link) still owes the ROB an answer: "this uop is done" and "here is a
// value" are separate facts (`value_valid`), and the completion is offered to
// the ROB either way.
//
// ---------------------------------------------------------- the ready table
//
// `q_*` answers, combinationally, "has this (tag, generation) been written?".
// Dispatch uses it at insert time to decide whether a source that was not ready
// when its uop was allocated has become durable while the uop waited for issue
// queue space. It is keyed on the generation, not the tag alone: a tag whose
// generation has moved on answers "not written" for the old generation, which is
// what stops a recycled tag being reported as ready for a producer that has not
// run yet.
//
// ------------------------------------------------------------ value stash
//
// The ROB does not carry values and the PRF's read ports belong to dispatch, so
// the durable value of each completion is kept here, indexed by the ROB index,
// for the retire payload to read back. It is a copy, not an authority: the
// retire path only looks at it for a macro the ROB says is complete and whose
// descriptor says it writes a register, and the PRF remains the architectural
// file for every other consumer.
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

// Widths are declared at file scope because a module's port list cannot see a
// module-body declaration. Every one is the contract's, not a re-derivation.
localparam int unsigned WBA_XLEN    = mosaic_cfg_pkg::MOSAIC_XLEN;
localparam int unsigned WBA_PRF_N   = mosaic_cfg_pkg::MOSAIC_INT_PRF_ENTRIES;
localparam int unsigned WBA_BANKS   = mosaic_cfg_pkg::MOSAIC_PRF_BANKS;
localparam int unsigned WBA_TAG_W   = mosaic_id_pkg::MOSAIC_ID_W_PRF_TAG;
localparam int unsigned WBA_PGEN_W  = mosaic_id_pkg::MOSAIC_ID_W_PRF_GEN;
// The generation width rename and the issue queue actually carry. rename's
// allocation generation is MOSAIC_INT_PRF_TAG_W bits and the IQ's TAG_GEN_W is
// the same value; the PRF's identity is 8 bits wide (MOSAIC_ID_W_PRF_GEN), so a
// destination generation is 8 bits on the wire and 7 bits in the structures that
// use it. The arbiter carries the 8-bit contract value and hands the low 7 bits
// to rename and to the wakeup, counting the case where the 8th bit is ever set
// rather than silently dropping it.
localparam int unsigned WBA_IGEN_W  = mosaic_cfg_pkg::MOSAIC_INT_PRF_TAG_W;  // 7
localparam int unsigned WBA_ROB_N   = mosaic_cfg_pkg::MOSAIC_ROB_ENTRIES;
localparam int unsigned WBA_IDX_W   = mosaic_id_pkg::MOSAIC_ID_W_ROB_INDEX;
localparam int unsigned WBA_RGEN_W  = mosaic_id_pkg::MOSAIC_ID_W_ROB_GEN;
localparam int unsigned WBA_UOP_W   = 3;   // $clog2(MOSAIC_MAX_UOPS_PER_MACRO), p0

module mosaic_wb_arbiter (
    input  logic                     clk,
    input  logic                     rst,

    // ------------------------------------------------- three completion producers
    // Each is valid/ready with the payload held stable while valid. `wb_ready`
    // is the arbiter's statement that it has taken the completion; a producer
    // that is refused holds it, so nothing can be lost to a full arbiter.
    input  mosaic_uop_pkg::wb_event_t wb_ev0,       // cluster 0
    input  logic                     wb_valid0,
    output logic                     wb_ready0,
    input  mosaic_uop_pkg::wb_event_t wb_ev1,       // cluster 1
    input  logic                     wb_valid1,
    output logic                     wb_ready1,
    input  mosaic_uop_pkg::wb_event_t wb_ev2,       // shared MUL/DIV
    input  logic                     wb_valid2,
    output logic                     wb_ready2,

    // -------------------------------------------------- PRF write ports (banks)
    output logic [WBA_BANKS-1:0]             prf_wr_en,
    output logic [WBA_BANKS-1:0]             prf_wr_gen_valid,
    output logic [WBA_BANKS*WBA_TAG_W-1:0]   prf_wr_tag,
    output logic [WBA_BANKS*WBA_PGEN_W-1:0]  prf_wr_gen,
    output logic [WBA_BANKS*WBA_XLEN-1:0]    prf_wr_data,

    // ------------------------------------------------- rename writeback (one)
    output logic                     ren_wb_valid,
    output logic [WBA_TAG_W-1:0]     ren_wb_tag,
    output logic [WBA_IGEN_W-1:0]    ren_wb_gen,
    input  logic                     ren_wb_accepted,
    input  logic                     ren_wb_stale,
    input  logic                     ren_wb_duplicate,

    // ------------------------------------------------------- ROB completion
    output logic                     rob_cmp_valid,
    output logic [WBA_IDX_W-1:0]     rob_cmp_index,
    output logic [WBA_RGEN_W-1:0]    rob_cmp_gen,
    output logic [WBA_UOP_W-1:0]     rob_cmp_uop,
    output logic                     rob_cmp_exc,
    input  logic                     rob_cmp_accepted,
    input  logic                     rob_cmp_duplicate,
    input  logic                     rob_cmp_stale,
    input  logic                     rob_cmp_bad_uop,
    // The memory path's classification of a completion (I-033..I-038). The
    // arbiter does not act on it, but it is carried so that a store's
    // completion -- an authorization with no register value -- can be counted
    // separately from a load's. The fields exist in the frozen packet and are
    // used here rather than suppressed.
    output logic [31:0]              o_store_ctr,
    output logic [31:0]              o_load_ctr,

    // ------------------------------------------------------ value-visible wakeup
    output logic                     wu_valid,
    output logic [WBA_TAG_W-1:0]     wu_tag,
    output logic [WBA_IGEN_W-1:0]    wu_gen,
    output logic [WBA_XLEN-1:0]      wu_val,

    // ------------------------------------------------------- ready-table query
    input  logic [1:0]               q_valid,
    input  logic [1:0][WBA_TAG_W-1:0] q_tag,
    input  logic [1:0][WBA_IGEN_W-1:0] q_gen,
    output logic [1:0]               q_written,

    // ------------------------------------------- durable value stash (retire)
    input  logic [WBA_IDX_W-1:0]     stash_rd0,
    input  logic [WBA_IDX_W-1:0]     stash_rd1,
    output logic                     stash_valid0,
    output logic [WBA_XLEN-1:0]      stash_value0,
    output logic                     stash_valid1,
    output logic [WBA_XLEN-1:0]      stash_value1,

    // ------------------------------------------------------ counters, reports
    output logic [31:0]              o_wr_ctr,
    output logic [31:0]              o_wake_ctr,
    output logic [31:0]              o_stale_ctr,
    output logic [31:0]              o_dup_ctr,
    output logic [31:0]              o_rob_stale_ctr,
    output logic [31:0]              o_rob_dup_ctr,
    output logic [31:0]              o_rob_ok_ctr,
    output logic [31:0]              o_collision_ctr,
    output logic [31:0]              o_drop_ctr,
    output logic [31:0]              o_pub_ctr,
    output logic [31:0]              o_wide_gen_ctr,
    output logic [31:0]              o_rob_bad_ctr,
    output logic                     o_pub_valid,
    output logic [WBA_IDX_W-1:0]     o_pub_index,
    output logic [WBA_RGEN_W-1:0]    o_pub_gen,
    output logic [WBA_XLEN-1:0]      o_pub_value
);

  // ------------------------------------------------------------- held events
  // One holding slot per producer: a producer can have at most one completion
  // outstanding (its own contract), so three slots are exactly enough and there
  // is no queue whose full/empty state could lose a result.
  mosaic_uop_pkg::wb_event_t pend_ev [0:2];
  logic [2:0]                pend_v;

  // The slot chosen to publish this cycle: lowest-numbered pending producer.
  logic [1:0]                sel;
  logic                      sel_found;

  always_comb begin
    sel_found = 1'b0;
    sel       = 2'd0;
    for (int unsigned i = 0; i < 3; i++) begin
      if (!sel_found && pend_v[i]) begin
        sel_found = 1'b1;
        sel       = 2'(i);
      end
    end
  end

  mosaic_uop_pkg::wb_event_t pub_ev;
  always_comb begin
    pub_ev = pend_ev[0];
    if (sel == 2'd1) pub_ev = pend_ev[1];
    if (sel == 2'd2) pub_ev = pend_ev[2];
  end

  logic publish;
  assign publish = sel_found;

  // A producer's slot is free unless it is occupied and not being published
  // this cycle: the publish frees its own slot, and the completion replacing it
  // can be taken in the same cycle.
  assign wb_ready0 = !pend_v[0] || (publish && (sel == 2'd0));
  assign wb_ready1 = !pend_v[1] || (publish && (sel == 2'd1));
  assign wb_ready2 = !pend_v[2] || (publish && (sel == 2'd2));

  // ------------------------------------------------------------- the decision
  // `dst_ok`  this completion owns a physical destination (not x0, has a value)
  // `wb_ok`   rename accepted the producer identity for that destination
  // `write_ok` the value is durable this cycle: PRF write, wakeup, stash
  logic dst_ok;
  logic ren_offered;
  logic rob_live;
  logic wb_ok;
  logic write_ok;

  assign dst_ok      = publish && pub_ev.value_valid && !pub_ev.dst.x0;
  assign ren_offered = dst_ok;
  // The ROB is the authority on identity liveness: a completion whose macro has
  // been discarded, or whose child index the slot does not own, is not a
  // producer of anything this machine will ever retire, so its value must not
  // reach a physical register. `cmp_accepted` is not required (a duplicate is
  // still a live macro), only that the identity is live.
  assign rob_live      = !rob_cmp_stale && !rob_cmp_bad_uop;
  assign wb_ok         = ren_offered && rob_live && ren_wb_accepted;

`ifdef MOSAIC_WB_MUTANT_EARLY_WAKE
  // NEGATIVE CONTROL: the wakeup is published for every completion with a
  // destination, whether or not the register file took the write. A consumer is
  // then told a value is available for an identity the PRF never stored, and it
  // computes with that value -- the card's "wakeup before the value is durable".
  assign write_ok = dst_ok;
`else
  assign write_ok = wb_ok;
`endif

  // ------------------------------------------------------------------- PRF
  // The home bank is `tag % MOSAIC_PRF_BANKS`, the same divisor rule
  // mosaic_prf.sv documents and implements. Only the selected destination's
  // bank is written; every other bank's port is idle.
  logic [WBA_TAG_W-1:0]           sel_tag;
  logic [WBA_PGEN_W-1:0]          sel_gen;
  logic [WBA_BANKS-1:0]           sel_bank;
  logic                           tag_in_range;

  assign sel_tag      = pub_ev.dst.tag;
  assign sel_gen      = pub_ev.dst.gen;
  assign tag_in_range = (32'(sel_tag) < 32'(WBA_PRF_N));
  assign sel_bank     = WBA_BANKS'(32'(sel_tag) % 32'(WBA_BANKS));

  always_comb begin
    prf_wr_en        = {WBA_BANKS{1'b0}};
    prf_wr_gen_valid = {WBA_BANKS{1'b0}};
    prf_wr_tag       = {(WBA_BANKS*WBA_TAG_W){1'b0}};
    prf_wr_gen       = {(WBA_BANKS*WBA_PGEN_W){1'b0}};
    prf_wr_data      = {(WBA_BANKS*WBA_XLEN){1'b0}};
    for (int unsigned b = 0; b < WBA_BANKS; b++) begin
      if (write_ok && tag_in_range && (sel_bank == WBA_BANKS'(b))) begin
        prf_wr_en[b]                    = 1'b1;
        prf_wr_gen_valid[b]             = 1'b1;
        prf_wr_tag[b*WBA_TAG_W +: WBA_TAG_W]   = sel_tag;
        prf_wr_gen[b*WBA_PGEN_W +: WBA_PGEN_W] = sel_gen;
        prf_wr_data[b*WBA_XLEN +: WBA_XLEN]    = pub_ev.value;
      end
    end
  end

  // --------------------------------------------------------------- rename
  // Only a completion that owns a destination is offered: rename's port is
  // about a physical tag, and offering an x0 completion would be reported as a
  // stale producer for a tag that has nothing to do with this instruction.
  assign ren_wb_valid = publish && ren_offered;
  assign ren_wb_tag   = sel_tag;
  assign ren_wb_gen   = sel_gen[WBA_IGEN_W-1:0];

  // ------------------------------------------------------------- ROB complete
  // Every completion owes the ROB an answer, destination or not.
  assign rob_cmp_valid = publish;
  assign rob_cmp_index = pub_ev.id.rob_index;
  assign rob_cmp_gen   = pub_ev.id.rob_gen;
  assign rob_cmp_uop   = pub_ev.id.uop_index;
  assign rob_cmp_exc   = pub_ev.exc.valid;

  // --------------------------------------------------------------- wakeup
  assign wu_valid = write_ok;
  assign wu_tag   = sel_tag;
  assign wu_gen   = sel_gen[WBA_IGEN_W-1:0];
  assign wu_val   = pub_ev.value;

  // ------------------------------------------------------------ ready table
  // {written, generation} per tag. Not reset except for the validity bits.
  logic [WBA_PRF_N-1:0]          rt_valid;
  logic [WBA_IGEN_W-1:0]         rt_gen [0:WBA_PRF_N-1];

  always_comb begin
    for (int unsigned q = 0; q < 2; q++) begin
      q_written[q] = 1'b0;
      if (q_valid[q] && (32'(q_tag[q]) < 32'(WBA_PRF_N))) begin
        q_written[q] = rt_valid[q_tag[q]] && (rt_gen[q_tag[q]] == q_gen[q]);
      end
    end
  end

  // -------------------------------------------------------------- value stash
  logic [WBA_ROB_N-1:0]   stash_valid_q;
  logic [WBA_XLEN-1:0]    stash_value_q [0:WBA_ROB_N-1];

  assign stash_valid0 = stash_valid_q[stash_rd0];
  assign stash_value0 = stash_valid_q[stash_rd0] ? stash_value_q[stash_rd0]
                                                 : {WBA_XLEN{1'b0}};
  assign stash_valid1 = stash_valid_q[stash_rd1];
  assign stash_value1 = stash_valid_q[stash_rd1] ? stash_value_q[stash_rd1]
                                                 : {WBA_XLEN{1'b0}};

  // --------------------------------------------------------- collision count
  // Two completions pending in one cycle whose destinations share a bank cannot
  // both be presented to the register file while rename has one writeback port;
  // the extra ones on a bank are the collisions that had to be delayed. Counted
  // on the pending set *before* this cycle's arrivals, so a cycle in which two
  // same-bank completions arrive is counted on the next cycle, when the second
  // is still waiting -- which is exactly the delay the case must show.
  logic [2:0]  bank_hits [0:WBA_BANKS-1];
  logic [31:0] extra_on_bank;
  logic [1:0]  pend_bank;

  always_comb begin
    for (int unsigned b = 0; b < WBA_BANKS; b++) bank_hits[b] = 3'd0;
    for (int unsigned i = 0; i < 3; i++) begin
      pend_bank = 2'(32'(pend_ev[i].dst.tag) % 32'(WBA_BANKS));
      if (pend_v[i]) bank_hits[pend_bank] = bank_hits[pend_bank] + 3'd1;
    end
    extra_on_bank = 32'd0;
    for (int unsigned b = 0; b < WBA_BANKS; b++) begin
      if (bank_hits[b] > 3'd1) extra_on_bank = extra_on_bank + (32'(bank_hits[b]) - 32'd1);
    end
  end

  // ----------------------------------------------------------------- counters
  logic [31:0] wr_ctr, wake_ctr, stale_ctr, dup_ctr, rob_stale_ctr, rob_dup_ctr;
  logic [31:0] collision_ctr, drop_ctr, pub_ctr, wide_gen_ctr;
  logic [31:0] store_ctr, load_ctr, rob_bad_ctr, rob_ok_ctr;

  always_ff @(posedge clk) begin
    if (rst) begin
      wr_ctr        <= 32'd0;
      wake_ctr      <= 32'd0;
      stale_ctr     <= 32'd0;
      dup_ctr       <= 32'd0;
      rob_stale_ctr <= 32'd0;
      rob_dup_ctr   <= 32'd0;
      collision_ctr <= 32'd0;
      drop_ctr      <= 32'd0;
      pub_ctr       <= 32'd0;
      wide_gen_ctr  <= 32'd0;
      store_ctr     <= 32'd0;
      load_ctr      <= 32'd0;
      rob_bad_ctr   <= 32'd0;
      rob_ok_ctr    <= 32'd0;
    end else begin
      wr_ctr        <= wr_ctr        + {31'd0, write_ok};
      wake_ctr      <= wake_ctr      + {31'd0, wu_valid};
      stale_ctr     <= stale_ctr     + {31'd0, publish && ren_offered && ren_wb_stale};
      dup_ctr       <= dup_ctr       + {31'd0, publish && ren_offered && ren_wb_duplicate};
      rob_stale_ctr <= rob_stale_ctr + {31'd0, publish && rob_cmp_stale};
      rob_dup_ctr   <= rob_dup_ctr   + {31'd0, publish && rob_cmp_duplicate};
      collision_ctr <= collision_ctr + extra_on_bank;
      drop_ctr      <= drop_ctr      + {31'd0, publish && ren_offered && !ren_wb_accepted};
      pub_ctr       <= pub_ctr       + {31'd0, publish};
      wide_gen_ctr  <= wide_gen_ctr  + {31'd0, publish && ren_offered && sel_gen[WBA_PGEN_W-1]};
      store_ctr     <= store_ctr     + {31'd0, publish && pub_ev.is_store};
      load_ctr      <= load_ctr      + {31'd0, publish && pub_ev.is_load};
      rob_bad_ctr   <= rob_bad_ctr   + {31'd0, publish && rob_cmp_bad_uop};
      rob_ok_ctr    <= rob_ok_ctr    + {31'd0, publish && rob_cmp_accepted};
    end
  end

  assign o_wr_ctr        = wr_ctr;
  assign o_wake_ctr      = wake_ctr;
  assign o_stale_ctr     = stale_ctr;
  assign o_dup_ctr       = dup_ctr;
  assign o_rob_stale_ctr = rob_stale_ctr;
  assign o_rob_dup_ctr   = rob_dup_ctr;
  assign o_collision_ctr = collision_ctr;
  assign o_drop_ctr      = drop_ctr;
  assign o_pub_ctr       = pub_ctr;
  assign o_wide_gen_ctr  = wide_gen_ctr;

  assign o_store_ctr   = store_ctr;
  assign o_load_ctr    = load_ctr;
  assign o_rob_bad_ctr = rob_bad_ctr;
  assign o_rob_ok_ctr  = rob_ok_ctr;

  // A "publication" is a completion that became durable: the value reached the
  // register file and the wakeup carried it. A completion the register file did
  // not take is reported by `o_pub_ctr` (processed) minus `o_wr_ctr` (written).
  assign o_pub_valid = write_ok;
  assign o_pub_index = pub_ev.id.rob_index;
  assign o_pub_gen   = pub_ev.id.rob_gen;
  assign o_pub_value = pub_ev.value;

  // -------------------------------------------------------------- next state
  always_ff @(posedge clk) begin
    if (rst) begin
      pend_v        <= 3'd0;
      rt_valid      <= {WBA_PRF_N{1'b0}};
      stash_valid_q <= {WBA_ROB_N{1'b0}};
    end else begin
      // Publish frees the selected slot; a new completion takes a slot whose
      // `wb_ready` was high.
      for (int unsigned i = 0; i < 3; i++) begin
        if (publish && (sel == 2'(i))) begin
          pend_v[i] <= 1'b0;
        end
      end
      if (wb_valid0 && wb_ready0) begin
        pend_ev[0] <= wb_ev0;
        pend_v[0]  <= 1'b1;
      end
      if (wb_valid1 && wb_ready1) begin
        pend_ev[1] <= wb_ev1;
        pend_v[1]  <= 1'b1;
      end
      if (wb_valid2 && wb_ready2) begin
        pend_ev[2] <= wb_ev2;
        pend_v[2]  <= 1'b1;
      end

      if (write_ok) begin
        rt_valid[sel_tag] <= 1'b1;
        rt_gen[sel_tag]   <= sel_gen[WBA_IGEN_W-1:0];
        stash_valid_q[pub_ev.id.rob_index] <= 1'b1;
        stash_value_q[pub_ev.id.rob_index] <= pub_ev.value;
      end
    end
  end

endmodule : mosaic_wb_arbiter

`default_nettype wire
