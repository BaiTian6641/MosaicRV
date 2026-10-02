// ============================================================================
// mosaic_mem_qos -- criticality/QoS memory arbitration (I-062).
//
// The MEF (memory fabric) scheduler decides which request gets the one memory
// request port. Three classes compete, and the classes are the requests' own
// identity -- what the request is, not a priority somebody handed it:
//
//   class | identity                          | policy role
//   ------+-----------------------------------+--------------------------------
//     0   | SCALAR: a latency-critical load    | reserved share (floor)
//     1   | PREFETCH: a prefetch (I-063)       | bounded share, placeholder
//     2   | BULK: streaming vector traffic     | quota (ceiling)
//
// A scalar dependent load (a load whose result feeds the next load's address)
// must not queue behind sustained bulk streaming. The card (implementation-plan
// section 3.1, I-062) names the three mechanisms this module implements and the
// two fail modes it must not ship:
//
//   * **age-based escape**: a request old enough (age >= QOS_AGE_LIMIT) in *any*
//     class is served over quota, regardless of class;
//   * **scalar reservation**: a minimum share per window for the latency-
//     critical class;
//   * **bulk quota**: a ceiling per window for the streaming class so it cannot
//     monopolise;
//   * PASS: every class meets a finite progress bound under the declared
//     service assumption, and the data result is unaffected by priority;
//   * FAIL: the scalar class permanently preempting bulk, or a performance
//     policy that changes memory ordering.
//
// -------------------------------------------------------- reuse of I-030
//
// I-030's `mosaic_arbiter` is the bounded-service engine for the *completion*
// path; this is the *memory request* scheduler, a different resource with a
// different geometry. What is reused is I-030's service model, not its
// instance:
//
//   * the windowed quota: a window is W grant opportunities' worth of grants,
//     `used[c] < Q[c]` is eligibility, the W-th grant wraps the window and
//     resets `used[]`;
//   * the exactness argument: a class with pending work throughout a window is
//     granted at least Q[c] times in it (see the proof below);
//   * the FIFO class queue with `req_ready` as backpressure and no
//     combinational path from the service port back into the producers;
//   * the checked downstream assumption (`QOS_D_MAX`) and its monitor.
//
// What I-062 *adds* is (a) the request's identity as the class, not a
// hand-set priority; (b) an **age-triggered** over-quota escape, where I-030's
// escape is triggered by the exhaustion of *every* quota; and (c) a hard bulk
// ceiling that the age trigger, not an always-on escape, relaxes. The
// consequence is stated honestly in the report: because the only over-quota
// path is the age one, the port idles when every pending class is at its
// ceiling and no request has aged, and that idle is bounded by AGE_LIMIT.
// I-030's immediate escape removes the idle at the cost of a looser ceiling.
//
// ------------------------------------------------------------- the rule
//
//   W      = sum of the quotas = 8. A **window** is W grants: the grant stream
//            is partitioned into consecutive groups of W grants (not W wall
//            cycles; a not-ready port or an idle stretch does not spend a
//            grant). `used[c]` is the grants charged to class c in the window.
//   Q[c]   the quota of class c: SCALAR 4, PREFETCH 2, BULK 2, W = 8.
//
//   1. **Eligibility.** Class c is eligible iff it has a pending request and
//      `used[c] < Q[c]`.
//   2. **Normal service.** If any class is eligible, the port serves the
//      *oldest pending request among the eligible classes* (smallest arrival
//      timestamp, ties on the lowest class index). Age first, not class index.
//   3. **Age escape.** Else, if some pending request has aged (`now - arrival
//      >= QOS_AGE_LIMIT`), the port serves the *oldest aged* request, over
//      quota, regardless of class. This is the only over-quota path: a request
//      that has aged is never held behind a ceiling.
//   4. **Idle.** Else the port is idle this cycle: every pending class is at
//      its ceiling and nothing has aged yet. The oldest pending request is
//      therefore at most AGE_LIMIT cycles from being served -- the idle is
//      bounded, and the window resumes when the request ages.
//   5. **Window wrap.** A grant charges `used[sel]` and the window's grant
//      count; the W-th grant wraps the window and clears `used[]`.
//
// Why the reservation is exact. In a window in which class c had pending work
// at every *grant*, suppose c received fewer than Q[c] grants. Then `used[c]`
// was below Q[c] whenever c was selected-or-not, so c was eligible at every
// opportunity that had a grant, so every grant was a normal (non-escape) grant
// and each was charged to a class under its quota. A non-c class can be charged
// at most Q[.] times in the window, so at most W - Q[c] grants were charged to
// classes other than c -- less than W, so c received at least Q[c] of the W.
// Contradiction. Hence **a class with pending work throughout a window is
// granted at least Q[c] times in it**, and a request with n requests of its own
// class ahead of it is granted within `ceil((n + 1) / Q[c]) + 1` windows. The
// age escape bounds the rest: a request that has not been served through its
// quota is served once it ages, so its wait is bounded by AGE_LIMIT plus the
// service of the requests older than it.
//
// ------------------------------------------------------------- the assumption
//
// Turning windows into wall-clock time needs the one thing this module cannot
// prove about its downstream, so it is written down and *checked*:
//
//   ASSUMPTION (bounded downstream). While the scheduler has pending work, the
//   port is not ready for at most QOS_D_MAX consecutive cycles; equivalently
//   the downstream becomes ready again within QOS_D_MAX cycles of going busy.
//   For the bulk class this is "memory returns within a bound".
//
// `o_busy_run_max` is the longest run of `pending work && !ready` the scheduler
// observed, and `o_assumption_violated` is set stickily until reset when that
// run exceeds QOS_D_MAX. A cycle count or "the simulation ended" is not a
// fairness proof: if the assumption is violated the bound computed from it is
// void for that stretch. Every waiting bound the case reports is
//   AGE_LIMIT + (D_MAX + 1) * ( N*DEPTH + W * (ceil(DEPTH / min Q) + 1) ),
// computed from this module's own constants and checked against the run.
//
// -------------------------------------------------------------- the control
//
// `qos_en` selects the policy. `qos_en = 1` is the rule above. `qos_en = 0` is
// the naive throughput-first rule the card forbids -- BULK, then PREFETCH, then
// SCALAR, with no quota and no age -- the control that shows the case's stimuli
// reach the fail mode (a sustained bulk stream starving the scalar chain). The
// class of a request never depends on `qos_en`; only the scheduling choice does,
// and the data each request carries is identical either way (the case checks
// the delivered data is equal with the policy off and on).
//
// ------------------------------------------------------------------ mutants
//
// Four `-DMOSAIC_QOS_MUTANT_*` defines inject one defect each; none is in the
// shipping build. The table with real output is in
// results/reports/I-062-memory-qos.md.
//
//   SCALAR_STARVE    the bulk class is never admitted (excluded from both the
//                    eligible and the aged set): the scalar class permanently
//                    takes the port and bulk starves -- the card's first fail
//                    mode.
//   BULK_MONOPOLY    the bulk class's eligibility ignores its ceiling, so bulk
//                    monopolises the port -- the card's "bulk quota removed".
//   NO_AGE_ESCAPE    nothing ever ages, so the only over-quota path is gone:
//                    when every pending class is at its ceiling the port idles
//                    for ever and a request waits without a bound.
//   DATA             the served request's data is corrupted, so a performance
//                    policy has changed a data result -- the card's second fail
//                    mode (an ordering/result change).
// ============================================================================

`default_nettype none
`resetall

module mosaic_mem_qos (
    input  logic             clk,
    input  logic             rst,

    // The policy selector. 1 = the QoS rule, 0 = the naive throughput-first
    // control. The classes do not depend on it.
    input  logic             qos_en,

    // ------------------------------------------------- request classes
    // One valid/ready port per class, class 0 in the low lane. `req_tag` is the
    // requester's identity, delivered with the grant; `req_data` is the
    // request's payload and is delivered unchanged (the case checks that the
    // data result does not depend on the scheduling choice).
    input  logic [2:0]       req_valid,
    output logic [2:0]       req_ready,
    input  logic [2:0][7:0]  req_tag,
    input  logic [2:0][31:0] req_data,

    // ------------------------------------------------------- service port
    // One request per ready opportunity. `srv_busy` is the memory's
    // backpressure: a grant transfer is `srv_go && !srv_busy`.
    output logic             srv_go,
    output logic [1:0]       srv_class,
    output logic [7:0]       srv_tag,
    output logic [31:0]      srv_data,
    input  logic             srv_busy,

    // -------------------------------------------------- window observation
    output logic [31:0]      o_window_index,
    output logic [31:0]      o_window_grants,
    output logic [2:0][31:0] o_used,
    output logic [2:0][31:0] o_pend_count,

    // --------------------------------------------------------- counters
    output logic [31:0]      o_grant_ctr,      // requests served
    output logic [31:0]      o_escape_ctr,     // age escapes (over-quota grants)
    output logic [31:0]      o_accept_ctr,     // requests admitted
    output logic [31:0]      o_refuse_ctr,     // refused admissions (backpressure)
    output logic [31:0]      o_stall_ctr,      // pending work, port not ready
    output logic [31:0]      o_idle_ctr,       // pending work, nothing eligible yet aged
    output logic [31:0]      o_drop_ctr,       // must stay zero
    output logic [31:0]      o_busy_run_max,   // longest !ready run with work
    output logic             o_assumption_violated,

    // -------------------------------------------------- geometry read-back
    output logic [31:0]      o_classes,
    output logic [31:0]      o_window,
    output logic [31:0]      o_d_max,
    output logic [31:0]      o_depth,
    output logic [31:0]      o_age_limit,
    output logic [2:0][31:0] o_quota
);

  // --------------------------------------------------------------- geometry
  // localparam, not parameter: the policy is one geometry and a caller-supplied
  // second one would be a second policy. A driver reads these back on the o_*
  // ports rather than restating them.
  localparam int unsigned QOS_N         = 3;
  localparam int unsigned QOS_DEPTH     = 4;
  localparam int unsigned QOS_TAGW      = 8;
  localparam int unsigned QOS_DATAW     = 32;
  localparam int unsigned QOS_TSW       = 32;
  localparam int unsigned QOS_QW        = 8;
  localparam int unsigned QOS_WINW      = 4;
  localparam int unsigned QOS_CNTW      = 3;
  localparam int unsigned QOS_CLSW      = 2;

  localparam int unsigned QOS_C_SCALAR   = 0;
  localparam int unsigned QOS_C_PREFETCH = 1;
  localparam int unsigned QOS_C_BULK     = 2;

  // The quotas. Their sum is the window: every grant in a window is charged to
  // the granted class, so the window is exactly as wide as the quotas.
  localparam int unsigned QOS_Q_SCALAR   = 4;
  localparam int unsigned QOS_Q_PREFETCH = 2;
  localparam int unsigned QOS_Q_BULK     = 2;
  localparam int unsigned QOS_WINDOW     = QOS_Q_SCALAR + QOS_Q_PREFETCH
                                         + QOS_Q_BULK;

  // The age trigger and the declared downstream assumption.
  localparam int unsigned QOS_AGE_LIMIT = 32;
  localparam int unsigned QOS_D_MAX     = 4;

  // The quotas as a vector, class 0 in the least significant slice, so the
  // selection loop can index them with a variable.
  localparam logic [QOS_N*QOS_QW-1:0] QOS_QUOTA =
      {QOS_QW'(QOS_Q_BULK), QOS_QW'(QOS_Q_PREFETCH), QOS_QW'(QOS_Q_SCALAR)};

  // --------------------------------------------------------------- the state
  // Queue storage. Validity lives entirely in `q_cnt`: an entry at or above the
  // count is data and nothing reads it.
  logic [QOS_DEPTH-1:0][QOS_TAGW-1:0]  q_tag_q  [QOS_N];
  logic [QOS_DEPTH-1:0][QOS_DATAW-1:0] q_data_q [QOS_N];
  logic [QOS_DEPTH-1:0][QOS_TSW-1:0]   q_ts_q   [QOS_N];
  logic [QOS_CNTW-1:0]                 q_cnt_q  [QOS_N];

  logic [QOS_DEPTH-1:0][QOS_TAGW-1:0]  q_tag_d  [QOS_N];
  logic [QOS_DEPTH-1:0][QOS_DATAW-1:0] q_data_d [QOS_N];
  logic [QOS_DEPTH-1:0][QOS_TSW-1:0]   q_ts_d   [QOS_N];
  logic [QOS_CNTW-1:0]                 q_cnt_d  [QOS_N];

  // The window's own state.
  logic [QOS_QW-1:0]   used_q [QOS_N];
  logic [QOS_WINW-1:0] win_grants_q;
  logic [31:0]         win_index_q;
  logic [QOS_TSW-1:0]  ts_q;

  // The assumption's state.
  logic [31:0] run_q;
  logic [31:0] run_max_q;
  logic        viol_q;

  // ------------------------------------------------------------- combinational
  logic [QOS_N-1:0] pend;
  logic [QOS_N-1:0] elig;
  logic [QOS_N-1:0] aged;
  logic [QOS_N-1:0] pick;
  logic [QOS_N-1:0] push_ok;
  logic             pend_any;
  logic             elig_any;
  logic             aged_any;
  logic             escape;
  logic             opp;
  logic             srv_go_t;
  logic             pop_tx;
  logic             charge;
  logic [QOS_CLSW-1:0] sel;
  logic             sel_found;
  logic [QOS_TSW-1:0]  sel_key;
  logic [QOS_TSW-1:0]  cand_key;
  logic [31:0]      run_next;
  logic [31:0]      refuse_now;
  logic [31:0]      accept_now;

  // A class queue is full at QOS_DEPTH; ready is a function of the registered
  // count alone (no combinational path from the service port, no same-cycle
  // slot reuse for a same-cycle push).
  always_comb begin
    push_ok    = 3'd0;
    pend       = 3'd0;
    refuse_now = 32'd0;
    accept_now = 32'd0;
    for (int unsigned c = 0; c < QOS_N; c = c + 1) begin
      req_ready[c] = (q_cnt_q[c] < QOS_CNTW'(QOS_DEPTH));
      pend[c]      = (q_cnt_q[c] != QOS_CNTW'(0));
      push_ok[c]   = req_valid[c] && req_ready[c];
      if (push_ok[c]) begin
        accept_now = accept_now + 32'd1;
      end
      if (req_valid[c] && !req_ready[c]) begin
        refuse_now = refuse_now + 32'd1;
      end
    end
    pend_any = |pend;
  end

  // Eligibility (under the ceiling) and age (past the trigger). The age escape
  // is the only over-quota path, so `aged` is what unlocks a spent ceiling.
  always_comb begin
    elig = 3'd0;
    aged = 3'd0;
    for (int unsigned c = 0; c < QOS_N; c = c + 1) begin
`ifndef MOSAIC_QOS_MUTANT_BULK_MONOPOLY
      elig[c] = pend[c] && (used_q[c] < QOS_QUOTA[c*QOS_QW +: QOS_QW]);
`else
      // MUTANT: the bulk class is eligible whenever it has work; its ceiling
      // is not consulted, so it can monopolise the port.
      if (c == QOS_C_BULK) begin
        elig[c] = pend[c];
      end else begin
        elig[c] = pend[c] && (used_q[c] < QOS_QUOTA[c*QOS_QW +: QOS_QW]);
      end
`endif
`ifdef MOSAIC_QOS_MUTANT_NO_AGE_ESCAPE
      // MUTANT: nothing ever ages, so the over-quota path is gone and an
      // all-at-ceiling state idles for ever.
      aged[c] = 1'b0;
`else
      aged[c] = pend[c] && ((ts_q - q_ts_q[c][0]) >= QOS_TSW'(QOS_AGE_LIMIT));
`endif
    end
    elig_any = |elig;
    aged_any = |aged;
  end

  // The selection. Normal service picks the oldest eligible request; the age
  // escape picks the oldest aged request; `qos_en = 0` is the naive
  // throughput-first control.
  always_comb begin
    pick = 3'd0;
    if (qos_en) begin
`ifdef MOSAIC_QOS_MUTANT_SCALAR_STARVE
      // MUTANT: the bulk class is never admitted -- not eligible and not aged
      // -- so the scalar class permanently takes the port and bulk starves.
      if (pend[QOS_C_SCALAR]) begin
        pick[QOS_C_SCALAR] = 1'b1;
      end else if (pend[QOS_C_PREFETCH]) begin
        pick[QOS_C_PREFETCH] = 1'b1;
      end
`else
      if (elig_any) begin
        pick = elig;
      end else if (aged_any) begin
        pick = aged;
      end
`endif
    end else begin
      // Naive throughput-first: the streaming class wins whenever it has work.
      if (pend[QOS_C_BULK]) begin
        pick[QOS_C_BULK] = 1'b1;
      end else if (pend[QOS_C_PREFETCH]) begin
        pick[QOS_C_PREFETCH] = 1'b1;
      end else begin
        pick[QOS_C_SCALAR] = pend[QOS_C_SCALAR];
      end
    end
  end

  // The oldest request among the pick set wins; ties on the lowest class index.
  // The arrival timestamps are 32 bits and a run is far shorter than half the
  // modulus, so the unsigned comparison is exact.
  always_comb begin
    sel_found = 1'b0;
    sel       = {QOS_CLSW{1'b0}};
    sel_key   = {QOS_TSW{1'b1}};
    for (int unsigned c = 0; c < QOS_N; c = c + 1) begin
      cand_key = q_ts_q[c][0];
      if (pick[c] && ((sel_found == 1'b0) || (cand_key < sel_key))) begin
        sel       = QOS_CLSW'(c);
        sel_key   = cand_key;
        sel_found = 1'b1;
      end
    end
  end

  always_comb begin
    opp      = sel_found && !srv_busy;
    srv_go_t = opp;
    pop_tx   = opp;
    // A grant is charged to the window on every transfer. `escape` is a grant
    // that was not eligibility-driven: the age escape.
    escape   = qos_en && sel_found && !elig_any;
    charge   = qos_en && opp;
    run_next = (pend_any && srv_busy) ? (run_q + 32'd1) : 32'd0;
  end

  assign srv_go    = srv_go_t;
  assign srv_class = sel;
  assign srv_tag   = q_tag_q[sel][0];
`ifdef MOSAIC_QOS_MUTANT_DATA
  // MUTANT: the payload delivered with a grant is corrupted, so the scheduling
  // choice has changed a data result -- the card's second fail mode.
  assign srv_data  = q_data_q[sel][0] ^ 32'h0000_00A5;
`else
  assign srv_data  = q_data_q[sel][0];
`endif

  // -------------------------------------------------------------- next state
  // Each class queue is rebuilt from its survivors (minus the granted head) and
  // then the newly accepted request, so a same-cycle pop and push cannot alias
  // and a refusal is never a loss.
  always_comb begin
    int unsigned wr;
    for (int unsigned c = 0; c < QOS_N; c = c + 1) begin
      wr = 0;
      for (int unsigned k = 0; k < QOS_DEPTH; k = k + 1) begin
        if (k < q_cnt_q[c]) begin
          if (!(pop_tx && (sel == QOS_CLSW'(c)) && (k == 0))) begin
            q_tag_d[c][wr]  = q_tag_q[c][k];
            q_data_d[c][wr] = q_data_q[c][k];
            q_ts_d[c][wr]   = q_ts_q[c][k];
            wr = wr + 1;
          end
        end
      end
      if (push_ok[c]) begin
        q_tag_d[c][wr]  = req_tag[c];
        q_data_d[c][wr] = req_data[c];
        q_ts_d[c][wr]   = ts_q;
        wr = wr + 1;
      end
      // Entries at or above the new count carry no meaning; they are cleared so
      // the whole vector is assigned on every path.
      for (int unsigned k = wr; k < QOS_DEPTH; k = k + 1) begin
        q_tag_d[c][k]  = {QOS_TAGW{1'b0}};
        q_data_d[c][k] = {QOS_DATAW{1'b0}};
        q_ts_d[c][k]   = {QOS_TSW{1'b0}};
      end
      q_cnt_d[c] = QOS_CNTW'(wr);
    end
  end

  // ----------------------------------------------------------------- the edge
  always_ff @(posedge clk) begin
    if (rst) begin
      ts_q         <= {QOS_TSW{1'b0}};
      win_grants_q <= {QOS_WINW{1'b0}};
      win_index_q  <= 32'd0;
      run_q        <= 32'd0;
      run_max_q    <= 32'd0;
      viol_q       <= 1'b0;
      o_grant_ctr  <= 32'd0;
      o_escape_ctr <= 32'd0;
      o_accept_ctr <= 32'd0;
      o_refuse_ctr <= 32'd0;
      o_stall_ctr  <= 32'd0;
      o_idle_ctr   <= 32'd0;
      o_drop_ctr   <= 32'd0;
      for (int unsigned c = 0; c < QOS_N; c = c + 1) begin
        q_cnt_q[c] <= QOS_CNTW'(0);
        used_q[c]  <= {QOS_QW{1'b0}};
      end
    end else begin
      ts_q <= ts_q + QOS_TSW'(1);

      for (int unsigned c = 0; c < QOS_N; c = c + 1) begin
        q_cnt_q[c] <= q_cnt_d[c];
        for (int unsigned k = 0; k < QOS_DEPTH; k = k + 1) begin
          q_tag_q[c][k]  <= q_tag_d[c][k];
          q_data_q[c][k] <= q_data_d[c][k];
          q_ts_q[c][k]   <= q_ts_d[c][k];
        end
      end

      // The window. A charged grant consumes one opportunity; the W-th grant
      // closes the window and opens the next one.
      if (charge) begin
        if (win_grants_q == QOS_WINW'(QOS_WINDOW - 1)) begin
          win_grants_q <= {QOS_WINW{1'b0}};
          win_index_q  <= win_index_q + 32'd1;
          for (int unsigned c = 0; c < QOS_N; c = c + 1) begin
            used_q[c] <= {QOS_QW{1'b0}};
          end
        end else begin
          win_grants_q <= win_grants_q + QOS_WINW'(1);
          used_q[sel]  <= used_q[sel] + QOS_QW'(1);
        end
      end

      // The counters.
      if (srv_go_t) begin
        o_grant_ctr <= o_grant_ctr + 32'd1;
        if (escape) begin
          o_escape_ctr <= o_escape_ctr + 32'd1;
        end
      end
      if (pop_tx && !srv_go_t) begin
        o_drop_ctr <= o_drop_ctr + 32'd1;
      end
      o_accept_ctr <= o_accept_ctr + accept_now;
      o_refuse_ctr <= o_refuse_ctr + refuse_now;
      if (pend_any && srv_busy) begin
        o_stall_ctr <= o_stall_ctr + 32'd1;
      end
      if (pend_any && !sel_found) begin
        o_idle_ctr <= o_idle_ctr + 32'd1;
      end

      // The assumption. `run_next` is this cycle's run length; the flag is
      // sticky until reset.
      run_q <= run_next;
      if (run_next > run_max_q) begin
        run_max_q <= run_next;
      end
      if (run_next > 32'(QOS_D_MAX)) begin
        viol_q <= 1'b1;
      end
    end
  end

  // ------------------------------------------------------------- observation
  assign o_window_index  = win_index_q;
  assign o_window_grants = 32'(win_grants_q);
  assign o_busy_run_max  = run_max_q;
  assign o_assumption_violated = viol_q;
  assign o_classes   = 32'(QOS_N);
  assign o_window    = 32'(QOS_WINDOW);
  assign o_d_max     = 32'(QOS_D_MAX);
  assign o_depth     = 32'(QOS_DEPTH);
  assign o_age_limit = 32'(QOS_AGE_LIMIT);

  for (genvar c = 0; c < QOS_N; c = c + 1) begin : g_observe
    assign o_used[c]       = 32'(used_q[c]);
    assign o_pend_count[c] = 32'(q_cnt_q[c]);
    assign o_quota[c]      = 32'(QOS_QUOTA[c*QOS_QW +: QOS_QW]);
  end

endmodule : mosaic_mem_qos

`default_nettype wire
