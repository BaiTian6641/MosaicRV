// ============================================================================
// mosaic_owner_fsm -- the resource-ownership change FSM (work package I-031).
//
// The fabric exists to be reconfigured: a pool of execution resources (issue
// queues, function units, result credits, network credits) that one owner holds
// can be handed to another owner while the machine keeps running. What makes
// that safe is not the handshake itself but the *order* of it: no new work may
// be admitted for the old owner after the change begins, every uop, result and
// credit the old owner still has outstanding must be settled, and only then may
// an id or a credit of the old owner be recycled under the new generation.
// Every one of those is a statement about state that no participant can check
// for itself -- a requester cannot see another requester's outstanding credit --
// so a single broker owns the transition and the participants obey it.
//
// This module is that broker. It is deliberately not a reset: a reset throws
// work away, and the contract here is that the old owner's work is *accounted*
// for, not discarded. It is also deliberately not the cold-start allocator
// (`mosaic_lease_alloc`, I-024): that hands out fresh leases, this changes who
// owns the pool the leases come from. The two share the generation idiom.
//
// ------------------------------------------------------------------ contract
//
// The state machine is the one frozen in the reconfigure interface
// (config/contracts/interfaces.json):
//
//   STOP_ADMIT -> DRAIN -> ACK -> PUBLISH -> RESUME
//
//   * A **control message** (`ctrl_valid` + `ctrl_seq`) asks for one ownership
//     change. Its `ctrl_seq` is its identity: the broker remembers the last
//     accepted sequence and a message repeating it is the *same* message --
//     `ctrl_dup` is raised, and nothing at all happens, whatever state the FSM
//     is in. This is what makes a held or retried request idempotent: it exists
//     so that a controller that cannot tell whether its message arrived may
//     simply send it again rather than build a second, stateful protocol on
//     top. A message with a *new* sequence arriving while the FSM is busy is
//     refused (`ctrl_reject`), not queued: the requester must observe
//     `ctrl_ready` and retry, so a reconfiguration can never be initiated from
//     inside another one.
//
//   * **STOP_ADMIT.** On acceptance `o_stop_admit` rises and stays high through
//     STOP, DRAIN and ACK. While it is high the participants must not begin new
//     work for the old owner. The FSM does not trust that promise silently: an
//     admit that arrives anyway is *still counted as outstanding* and raises
//     `o_admit_after_stop_count`. Refusing it instead would be the classic
//     lost-work defect -- the fabric would have work the broker does not know
//     about, and the broker would publish a new owner while it is in flight.
//     Counting it costs one comparison; dropping it loses architectural state.
//
//   * **DRAIN.** The FSM waits until all three outstanding counts are zero.
//     They are separate because they are separate obligations: a uop can leave
//     the issue queue and still be executing, a result can be executing and not
//     yet collected, and a result credit can be held by a writeback path the
//     queue has never seen. "The issue queue is empty" is *not* "the pool is
//     drained", which is the card's stated failure mode. Each class has its own
//     `*_new` / `*_done` event pair; a `*_done` for an empty class is
//     *unmatched* (`o_settle_unmatched_count`) and changes nothing, so a
//     double-settle cannot fold one class's debt into another's.
//
//   * A watchdog bounds the drain. Each outstanding item can settle at most
//     once per cycle and the outstanding population is bounded by the ROB, so
//     `DRAIN_LIMIT` cycles (see below) is far more than any honest drain needs.
//     If it expires the reconfiguration is **aborted**, not completed: the FSM
//     returns to IDLE with `o_owner_gen` unchanged, `o_publish` never pulses,
//     and `o_drain_stall` latches. That is the frozen contract's cancel clause
//     ("an aborted reconfiguration returns the resource to its original owner
//     with its original generation"). A lost credit therefore cannot cause a
//     hang or a wrong publish; it causes a loud, safe abort.
//
//   * **ACK.** Drain reaching zero is a fact the *broker* observes; the barrier
//     is a fact the *participants* must state. `o_ack_req` rises and the FSM
//     waits for `ack_valid`. Only then may ids, credits and state of the old
//     owner be reused, so a participant that has not yet acknowledged cannot
//     have a late settle land on the new owner. An `ack_valid` in any other
//     state is `ack_unexpected`, counted and ignored -- an ack that arrives late
//     or twice must not advance a generation.
//
//   * **PUBLISH.** In this one cycle `o_publish` is high, `o_owner_gen` is
//     already the new generation (`o_old_gen` + 1), and `o_stop_admit` is low:
//     RESUME. The new generation always differs from the old one by exactly one,
//     and it is published only from this state, which is reachable only through
//     DRAIN and ACK. That reachability *is* the guarantee; there is no second
//     path that writes `o_owner_gen`.
//
// ------------------------------------------------------- where the generations
// ------------------------------------------------------- come from
//
// `o_owner_gen` is the generation the participants stamp on every id and credit
// of the current owner. Its width is `clog2(rob_entries) + 1`, the same idiom
// `mosaic_iq` uses for wakeup generations and `mosaic_lease_alloc` for lease
// generations, and for the same reason: an id recycled under a new owner must
// not alias an old id still in flight. The bound is generous here because the
// publish is gated on a full drain, so at the instant of publish there is no old
// id outstanding at all. `o_old_gen` and `o_new_gen` are meaningful while
// `o_busy`: they name the change in progress.
//
// The drain count is `clog2(rob_entries * max_uops_per_macro + 1)` wide, exactly
// the contract's `outstanding` field: every uop of every live ROB entry, which
// is the largest population the pool can hold. A counter saturates rather than
// wrapping, so a stimulus that exceeded the architectural bound would report a
// full counter and fail the shadow comparison instead of silently aliasing.
//
// ------------------------------------------------------------------ mutants
//
// `-DMOSAIC_OWNER_MUTANT_<name>` injects one defect used to prove the case can
// fail. The shipping build defines none of them. The table with real output is
// in results/reports/I-031-ownership.md.
// ============================================================================

`default_nettype none
`resetall

/* verilator lint_off UNUSEDPARAM */
// The generated header declares one localparam per configuration knob for the
// whole project; this module names four of them, and the rest are unused here by
// construction, not by omission.
`include "mosaic_cfg_pkg.svh"
/* verilator lint_on UNUSEDPARAM */

module mosaic_owner_fsm #(
  // Owner-generation width, from the frozen contract: `clog2(rob_entries)+1`.
  // `localparam`, not `parameter`: a caller cannot override a width into a
  // second generation scheme that disagrees with the profile the rest of the
  // core was built for.
  localparam int unsigned GEN_W = $clog2(mosaic_cfg_pkg::MOSAIC_ROB_ENTRIES) + 1,

  // Drain count width: the contract's `outstanding`, every uop of every live ROB
  // entry.
  localparam int unsigned CNT_W = $clog2(mosaic_cfg_pkg::MOSAIC_ROB_ENTRIES
                                         * mosaic_cfg_pkg::MOSAIC_MAX_UOPS_PER_MACRO + 1),

  // Control-message sequence width. Fixed by the interface, not by the profile:
  // a message identity is a small wrapping label, and the contract only asks
  // that two consecutive reconfigurations differ.
  localparam int unsigned SEQ_W = 8,

  // State encoding needs three bits for five states.
  localparam int unsigned ST_W  = 3,

  // The drain watchdog. A drain settles at least one outstanding item per cycle
  // in the honest case and the population is bounded by the ROB, so the
  // architectural bound is MOSAIC_ROB_ENTRIES cycles; four times that is the
  // slack before an abort is declared. It is a *bound*, not a timeout used as a
  // proof: the case measures the real drain latency and checks it against the
  // geometry, and the watchdog is what turns a lost credit into a named abort.
  localparam int unsigned DRAIN_LIMIT = 4 * mosaic_cfg_pkg::MOSAIC_ROB_ENTRIES
) (
  input  logic                 clk,
  input  logic                 rst,

  // ------------------------------------------------------ control message
  // One message = one ownership change. `ctrl_seq` is its identity.
  input  logic                 ctrl_valid,
  input  logic [SEQ_W-1:0]     ctrl_seq,
  output logic                 ctrl_ready,       // the FSM may accept this cycle
  output logic                 ctrl_ok,          // accepted this cycle: the change starts
  output logic                 ctrl_dup,         // repeats the last accepted sequence
  output logic                 ctrl_reject,      // a new sequence while busy

  // --------------------------------------------------- barrier acknowledgement
  input  logic                 ack_valid,
  output logic                 ack_req,          // high in ACK: participants must ack
  output logic                 ack_ok,           // accepted this cycle
  output logic                 ack_unexpected,   // ack in any other state

  // -------------------------------------------- outstanding-work event pairs
  // Independent per class, because they are independent obligations. The FSM
  // counts an admit even while admitting is stopped (and flags it), so no work
  // can be lost by being refused after the barrier has begun.
  input  logic                 uop_new,
  input  logic                 uop_done,
  input  logic                 res_new,
  input  logic                 res_done,
  input  logic                 crd_new,
  input  logic                 crd_done,

  // ------------------------------------------------------------- state
  output logic [ST_W-1:0]      o_state,
  output logic                 o_stop_admit,
  output logic                 o_busy,
  output logic                 o_publish,        // one-cycle pulse in PUBLISH
  output logic [GEN_W-1:0]     o_owner_gen,      // the generation in force now
  output logic [GEN_W-1:0]     o_old_gen,        // meaningful while o_busy
  output logic [GEN_W-1:0]     o_new_gen,        // meaningful while o_busy

  // --------------------------------------------------------- observation
  output logic [CNT_W-1:0]     o_cnt_uop,
  output logic [CNT_W-1:0]     o_cnt_res,
  output logic [CNT_W-1:0]     o_cnt_crd,
  output logic [CNT_W-1:0]     o_drain_cycles,
  output logic                 o_drain_stall,    // a drain aborted: sticky until reset
  output logic [31:0]          o_ctrl_ok_count,
  output logic [31:0]          o_ctrl_dup_count,
  output logic [31:0]          o_ctrl_reject_count,
  output logic [31:0]          o_ack_ok_count,
  output logic [31:0]          o_ack_unexpected_count,
  output logic [31:0]          o_admit_after_stop_count,
  output logic [31:0]          o_settle_unmatched_count,
  output logic [31:0]          o_publish_count,
  output logic [31:0]          o_abort_count,

  // ------------------------------------------------- geometry readback
  output logic [31:0]          o_gen_w,
  output logic [31:0]          o_cnt_w,
  output logic [31:0]          o_seq_w,
  output logic [31:0]          o_drain_limit,
  output logic [31:0]          o_states
);

  // ------------------------------------------------------------- encodings
  localparam logic [ST_W-1:0] S_IDLE    = 3'd0;
  localparam logic [ST_W-1:0] S_STOP    = 3'd1;
  localparam logic [ST_W-1:0] S_DRAIN   = 3'd2;
  localparam logic [ST_W-1:0] S_ACK     = 3'd3;
  localparam logic [ST_W-1:0] S_PUBLISH = 3'd4;
  localparam int unsigned     STATE_COUNT = 5;

  localparam logic [CNT_W-1:0] CNT_ZERO = {CNT_W{1'b0}};
  localparam logic [CNT_W-1:0] CNT_ONE  = {{(CNT_W - 1){1'b0}}, 1'b1};
  localparam logic [CNT_W-1:0] CNT_MAX  = {CNT_W{1'b1}};
  localparam logic [CNT_W-1:0] DRAIN_LAST = CNT_W'(DRAIN_LIMIT - 1);
  localparam logic [GEN_W-1:0] GEN_ONE  = {{(GEN_W - 1){1'b0}}, 1'b1};

  // ------------------------------------------------------------- state
  logic [ST_W-1:0]  state;
  logic [GEN_W-1:0] owner_gen;
  logic [GEN_W-1:0] old_gen;
  logic [SEQ_W-1:0] last_seq;
  logic             seq_seen;

  logic [CNT_W-1:0] cnt_uop;
  logic [CNT_W-1:0] cnt_res;
  logic [CNT_W-1:0] cnt_crd;
  logic [CNT_W-1:0] drain_ctr;
  logic             drain_stall;

  logic [31:0]      ctrl_ok_count;
  logic [31:0]      ctrl_dup_count;
  logic [31:0]      ctrl_reject_count;
  logic [31:0]      ack_ok_count;
  logic [31:0]      ack_unexpected_count;
  logic [31:0]      admit_after_stop_count;
  logic [31:0]      settle_unmatched_count;
  logic [31:0]      publish_count;
  logic [31:0]      abort_count;

  // ------------------------------------------------- combinational answers
  assign o_state      = state;
  assign o_busy       = (state != S_IDLE);
  assign o_publish    = (state == S_PUBLISH);
  assign o_owner_gen  = owner_gen;
  assign o_old_gen    = old_gen;
  assign o_new_gen    = old_gen + GEN_ONE;
  assign o_cnt_uop    = cnt_uop;
  assign o_cnt_res    = cnt_res;
  assign o_cnt_crd    = cnt_crd;
  assign o_drain_cycles = drain_ctr;
  assign o_drain_stall  = drain_stall;

  assign o_gen_w       = GEN_W;
  assign o_cnt_w       = CNT_W;
  assign o_seq_w       = SEQ_W;
  assign o_drain_limit = DRAIN_LIMIT;
  assign o_states      = STATE_COUNT;

  assign o_ctrl_ok_count          = ctrl_ok_count;
  assign o_ctrl_dup_count         = ctrl_dup_count;
  assign o_ctrl_reject_count      = ctrl_reject_count;
  assign o_ack_ok_count           = ack_ok_count;
  assign o_ack_unexpected_count   = ack_unexpected_count;
  assign o_admit_after_stop_count = admit_after_stop_count;
  assign o_settle_unmatched_count = settle_unmatched_count;
  assign o_publish_count          = publish_count;
  assign o_abort_count            = abort_count;

  // Admitting is stopped from STOP through ACK. It is *not* stopped in the
  // accept cycle itself (the message has only just arrived, and any admit
  // presented that cycle is the last legal one for the old owner) nor in
  // PUBLISH (the new owner is already in force -- that is RESUME).
  assign o_stop_admit = (state == S_STOP) || (state == S_DRAIN) || (state == S_ACK);

  // `admit_stopped` names the same fact for the counter logic below.
  wire admit_stopped = o_stop_admit;

  // ------------------------------------------------------ control identity
  // A message is a duplicate iff `seq_seen` and its sequence is the last one
  // accepted. The very first message is never a duplicate, whatever sequence it
  // carries, so a controller may start at zero.
  logic ctrl_is_dup;
  assign ctrl_is_dup = seq_seen && (ctrl_seq == last_seq);

  assign ctrl_ready  = (state == S_IDLE);
`ifdef MOSAIC_OWNER_MUTANT_REPEAT_CTRL
  // The idempotency defect: the accept test no longer compares the sequence, so
  // a held or retried message *re-executes* while the duplicate is still
  // reported. A held message then starts a second reconfiguration and the
  // generation advances twice for one request.
  assign ctrl_ok     = ctrl_valid && (state == S_IDLE);
  assign ctrl_dup    = ctrl_valid && ctrl_is_dup;
`else
  assign ctrl_ok     = ctrl_valid && (state == S_IDLE) && !ctrl_is_dup;
  assign ctrl_dup    = ctrl_valid && ctrl_is_dup;
`endif
  assign ctrl_reject = ctrl_valid && (state != S_IDLE) && !ctrl_is_dup;

  // ------------------------------------------------------------- barrier
  assign ack_req        = (state == S_ACK);
  assign ack_ok         = ack_valid && (state == S_ACK);
  assign ack_unexpected = ack_valid && (state != S_ACK);

  // --------------------------------------------------- outstanding counters
  // Each class has one count. An admit raises it and a settle lowers it, and a
  // *same-cycle* admit and settle cancel (the settle consumes the admit), which
  // is why the admit is folded into `*_avail` before the settle is applied --
  // the count is the stack depth, not two independent tallies. A settle with
  // nothing available is unmatched: it is counted and applies nothing, so a
  // double-settle cannot borrow against another class. An admit on a full class
  // saturates; the width is the architectural population bound, so a legal
  // stimulus never reaches it and an illegal one fails the shadow comparison
  // rather than wrapping to a small number.
  //
  // Every counter gets exactly *one* non-blocking assignment. Three sequential
  // `counter <= counter + 1` statements would not add three: each reads the old
  // value and the last wins, so the unmatched and admit accounting are summed
  // combinationally and applied once.
  wire uop_up;
  wire res_up;
  wire crd_up;
`ifdef MOSAIC_OWNER_MUTANT_DROP_ADMIT_AFTER_STOP
  // Refusing an admit after the stop would lose work the fabric has already
  // begun: the counter would not know about it and the new owner would be
  // published while it is still in flight.
  assign uop_up = uop_new && !admit_stopped;
  assign res_up = res_new && !admit_stopped;
  assign crd_up = crd_new && !admit_stopped;
`else
  assign uop_up = uop_new;
  assign res_up = res_new;
  assign crd_up = crd_new;
`endif

  logic [CNT_W-1:0] uop_avail;
  logic [CNT_W-1:0] res_avail;
  logic [CNT_W-1:0] crd_avail;
  assign uop_avail = (uop_up && (cnt_uop != CNT_MAX)) ? (cnt_uop + CNT_ONE) : cnt_uop;
  assign res_avail = (res_up && (cnt_res != CNT_MAX)) ? (cnt_res + CNT_ONE) : cnt_res;
`ifdef MOSAIC_OWNER_MUTANT_DOUBLE_CREDIT
  // One admitted credit charged twice: the count runs ahead of the debt.
  assign crd_avail = (crd_up && (cnt_crd <= (CNT_MAX - CNT_ONE)))
                   ? (cnt_crd + CNT_ONE + CNT_ONE) : cnt_crd;
`else
  assign crd_avail = (crd_up && (cnt_crd != CNT_MAX)) ? (cnt_crd + CNT_ONE) : cnt_crd;
`endif

  logic [CNT_W-1:0] nxt_uop;
  logic [CNT_W-1:0] nxt_res;
  logic [CNT_W-1:0] nxt_crd;
  logic             uop_unmatched;
  logic             res_unmatched;
  logic             crd_unmatched;
  always_comb begin
    uop_unmatched = uop_done && (uop_avail == CNT_ZERO);
    nxt_uop = (uop_done && (uop_avail != CNT_ZERO)) ? (uop_avail - CNT_ONE) : uop_avail;

    res_unmatched = res_done && (res_avail == CNT_ZERO);
    nxt_res = (res_done && (res_avail != CNT_ZERO)) ? (res_avail - CNT_ONE) : res_avail;

    crd_unmatched = crd_done && (crd_avail == CNT_ZERO);
`ifdef MOSAIC_OWNER_MUTANT_LOST_CREDIT
    // A returned credit that never decrements the credit count: the outstanding
    // debt grows without bound and the drain can never complete.
    nxt_crd = crd_avail;
`else
    nxt_crd = (crd_done && (crd_avail != CNT_ZERO)) ? (crd_avail - CNT_ONE) : crd_avail;
`endif
  end

  // Admits that arrived while admitting was stopped, summed in one place so the
  // one assignment to the counter adds all of them.
  logic [1:0] admit_inc;
  always_comb begin
    admit_inc = {1'b0, admit_stopped && uop_new}
              + {1'b0, admit_stopped && res_new}
              + {1'b0, admit_stopped && crd_new};
  end

  logic [1:0] unmatched_inc;
  always_comb begin
    unmatched_inc = {1'b0, uop_unmatched} + {1'b0, res_unmatched} + {1'b0, crd_unmatched};
  end

  // Drain is complete only when *every* class is empty. This is the line the
  // card's failure mode ("the issue queue is empty, so the pool is drained")
  // would replace with `cnt_uop == 0` alone.
`ifdef MOSAIC_OWNER_MUTANT_PUBLISH_EARLY
  // Publishing before the drain completes: only the uop class is waited for,
  // so an outstanding result or credit is abandoned by the old owner.
  wire drained = (cnt_uop == CNT_ZERO);
`else
  wire drained = (cnt_uop == CNT_ZERO) && (cnt_res == CNT_ZERO) && (cnt_crd == CNT_ZERO);
`endif

  wire drain_expired = (drain_ctr >= DRAIN_LAST);

  // ------------------------------------------------------------- registers
  always_ff @(posedge clk) begin
    if (rst) begin
      state       <= S_IDLE;
      owner_gen   <= {GEN_W{1'b0}};
      old_gen     <= {GEN_W{1'b0}};
      last_seq    <= {SEQ_W{1'b0}};
      seq_seen    <= 1'b0;
      cnt_uop     <= CNT_ZERO;
      cnt_res     <= CNT_ZERO;
      cnt_crd     <= CNT_ZERO;
      drain_ctr   <= CNT_ZERO;
      drain_stall <= 1'b0;
      ctrl_ok_count          <= 32'd0;
      ctrl_dup_count         <= 32'd0;
      ctrl_reject_count      <= 32'd0;
      ack_ok_count           <= 32'd0;
      ack_unexpected_count   <= 32'd0;
      admit_after_stop_count <= 32'd0;
      settle_unmatched_count <= 32'd0;
      publish_count          <= 32'd0;
      abort_count            <= 32'd0;
    end else begin
      cnt_uop <= nxt_uop;
      cnt_res <= nxt_res;
      cnt_crd <= nxt_crd;

      // An admit while admitting is stopped is counted as outstanding and
      // flagged. It is *not* refused: work the fabric has begun must be settled
      // before the new owner is published, and refusing it here would be the
      // lost-work defect this module exists to prevent. One assignment adds the
      // whole cycle's admits, so two admits in one cycle are two, not one.
      if (admit_inc != 2'd0) begin
        admit_after_stop_count <= admit_after_stop_count + {30'd0, admit_inc};
      end

      // A settle with nothing outstanding is unmatched: counted, applied to no
      // count, so a double-settle cannot borrow against another class.
      if (unmatched_inc != 2'd0) begin
        settle_unmatched_count <= settle_unmatched_count + {30'd0, unmatched_inc};
      end

      if (ctrl_ok) begin
        last_seq <= ctrl_seq;
        seq_seen <= 1'b1;
        old_gen  <= owner_gen;
        ctrl_ok_count <= ctrl_ok_count + 32'd1;
      end
      if (ctrl_dup) begin
        ctrl_dup_count <= ctrl_dup_count + 32'd1;
      end
      if (ctrl_reject) begin
        ctrl_reject_count <= ctrl_reject_count + 32'd1;
      end

      if (ack_ok) begin
        ack_ok_count <= ack_ok_count + 32'd1;
      end
      if (ack_unexpected) begin
        ack_unexpected_count <= ack_unexpected_count + 32'd1;
      end

      case (state)
        S_IDLE: begin
          if (ctrl_ok) state <= S_STOP;
        end
        S_STOP: begin
          // Exactly one cycle of enforced no-admit, so that every admit
          // presented before the message was accepted is in the counters the
          // DRAIN state will read.
          state <= S_DRAIN;
        end
        S_DRAIN: begin
          if (drained) begin
`ifdef MOSAIC_OWNER_MUTANT_NO_ACK
            // The barrier defect: publish as soon as the drain looks complete,
            // without the participants' acknowledgement.
            state         <= S_PUBLISH;
            owner_gen     <= old_gen + GEN_ONE;
            publish_count <= publish_count + 32'd1;
`else
            state <= S_ACK;
`endif
          end else if (drain_expired) begin
            // Abort, do not complete: the old owner keeps its generation, no
            // publish occurs, and the stall is visible.
            state       <= S_IDLE;
            drain_stall <= 1'b1;
            abort_count <= abort_count + 32'd1;
          end
        end
        S_ACK: begin
          if (ack_valid) begin
            // Publish the new generation on the edge *into* PUBLISH, so that
            // `o_publish`, the new `o_owner_gen` and the publish tally are all
            // visible together in the PUBLISH cycle.
            state         <= S_PUBLISH;
            owner_gen     <= old_gen + GEN_ONE;
            publish_count <= publish_count + 32'd1;
          end
        end
        S_PUBLISH: begin
          state <= S_IDLE;
        end
        default: begin
          state <= S_IDLE;
        end
      endcase

      if (state == S_DRAIN) begin
        if (!drained && !drain_expired) drain_ctr <= drain_ctr + CNT_ONE;
      end else begin
        drain_ctr <= CNT_ZERO;
      end
    end
  end

endmodule : mosaic_owner_fsm

`resetall
`default_nettype wire
