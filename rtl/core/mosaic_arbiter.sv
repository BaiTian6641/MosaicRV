// ============================================================================
// mosaic_arbiter -- quota-bounded service with reserved escape capacity (I-030).
//
// One service port serves four classes of completion traffic. The port is the
// scarce resource: the writeback/return path can retire one completion per
// ready cycle, and the classes compete for it. A plain priority scheme makes
// the loudest class win for ever; this module is the rule that bounds every
// class's waiting instead.
//
// The card (docs/implementation-plan.md section 6, I-030) states the obligation
// precisely:
//
//   * reserve escape capacity for the ready-oldest and for the return channel;
//   * write the fairness assumptions down;
//   * under an explicitly stated assumption ("the downstream eventually becomes
//     ready / memory returns within a bound") there is a computable waiting
//     bound, covered by assertions and a stress case;
//   * the fail modes are an infinite timeout used as a fairness proof, and a
//     high-priority scalar stream starving the vector class for ever.
//
// ------------------------------------------------------------- the classes
//
//   index | class  | what it carries                          | service
//   ------+--------+------------------------------------------+--------
//     0   | SCALAR | the integer ALU/branch completions        | short
//     1   | MEMORY | the return channel: load/store completions| short
//     2   | MULDIV | the shared iterative MUL/DIV completions  | long
//     3   | VECTOR | the wide/long-latency completions         | long
//
// The classes are arbitration classes of the completion path, not ISA classes:
// the mapping above is stated so the case's environment is the same machine the
// plan describes, but nothing here decodes an instruction.
//
// ---------------------------------------------------------- the policy rule
//
// The policy is a *windowed quota with a reserved minimum*, and it is stated as
// a rule because the bound is derived from it and from nothing else:
//
//   W  = sum of the four quotas. A **window** is W grant opportunities -- W
//        cycles in which the arbiter has pending work and the port is ready.
//        W cycles, not W wall cycles: a busy port does not consume the window.
//   Q[c] the quota of class c: 4, 2, 1, 1 for SCALAR..VECTOR, W = 8.
//   used[c]  grants to class c in the current window.
//
//   1. **Eligibility.** Class c is eligible iff it has a pending request and
//      `used[c] < Q[c]`.
//   2. **Normal service.** If any class is eligible, the port serves the
//      *oldest pending request* among the eligible classes -- smallest arrival
//      timestamp, ties on the lowest class index. Age, not class index, so the
//      ready-oldest is served first.
//   3. **Escape.** If no class is eligible but some class has pending work, the
//      port serves the **oldest pending request** among all classes, over
//      quota. This is the reserved escape capacity: a class cannot be blocked
//      by the exhaustion of every quota, and the globally oldest request always
//      has a path to service.
//   4. **Window advance.** On a grant, `used[sel]` and the window's grant count
//      advance. After the W-th grant of a window the window wraps: `used[]` and
//      the count reset, `o_window_index` increments. Every grant is charged to
//      the granted class's quota, so the W opportunities of a window are
//      partitioned as `sum_c Q[c] = W`.
//
// Why the reservation is exact, stated as the invariant the case checks:
//
//   In any window, the grants charged to classes other than c number at most
//   `W - Q[c]`, because each such grant is charged to its own class's quota and
//   a class can be charged at most Q[.] times in a window. An escape grant can
//   only happen when *no* class with pending work is under its quota, so while
//   class c is under quota and has work no escape occurs and no non-c grant is
//   uncharged. Therefore a class that has pending work throughout a window is
//   granted **at least Q[c]** times in it. A request with `n` requests of its
//   own class ahead of it is therefore granted within
//
//       ceil((n + 1) / Q[c]) + 1   windows,
//
//   the extra window because the arrival may land at the end of a window whose
//   quota is already spent.
//
// ------------------------------------------------------------- the assumption
//
// The bound above is in *windows*, and a window is W grant opportunities. The
// wall-clock bound needs the one thing this module cannot prove about its
// downstream, and it is therefore written down as an assumption and *checked*
// rather than assumed quietly:
//
//     ASSUMPTION (bounded downstream). While the arbiter has pending work, the
//     port is not ready for at most ARB_D_MAX consecutive cycles; equivalently
//     the downstream becomes ready again within ARB_D_MAX cycles of going busy.
//     For the memory class this is "memory returns within a bound".
//
// `o_busy_run_max` is the longest run of `pending work && !ready` the arbiter
// actually observed, and `o_assumption_violated` is set -- stickily, until
// reset -- when that run exceeds ARB_D_MAX. A cycle count, an infinite timeout
// or a "the simulation ended" is *not* a fairness proof: if the assumption is
// violated the arbiter reports a violation, and the waiting bound computed from
// it is void for that stretch. The case asserts both directions: the shipping
// stimulus never trips the flag, and a directed hold of the port trips it.
//
// ------------------------------------------------------------- the credits
//
// Each class owns a queue of ARB_DEPTH entries. `req_ready[c]` is low when the
// queue is full, and that is the *backpressure*: a refused request is held by
// its producer and offered again, never sampled and never dropped. The queues
// are FIFO within a class, so the requests ahead of a given one are exactly the
// ones accepted before it, and `o_pend_count` reports the occupancy the bound's
// `n` is measured from. A completion frees one entry; a full queue is a refusal
// and not a loss, and `o_drop_ctr` exists to stay at zero.
//
// `req_ready` deliberately does not consult `srv_busy`, so there is no
// combinational path from the downstream back into the producers, and a
// same-cycle grant does not free a slot for a same-cycle push. A refused push
// waits one cycle; nothing is lost by waiting.
//
// ------------------------------------------------------------------ mutants
//
// Three `-DMOSAIC_ARB_MUTANT_*` defines inject one defect each. None is defined
// in the shipping build. The table with real output is in
// results/reports/I-030-forward-progress.md:
//
//   STARVE          eligibility ignores the quota and the choice is the lowest
//                   class index: strict class priority. The SCALAR stream then
//                   serves for ever and VECTOR never runs -- the card's fail
//                   mode, stated as a defect.
//   DROP_ON_QUOTA   when no class is under quota the selected request is popped
//                   and *discarded* instead of delivered: loss instead of
//                   backpressure.
//   REQ_ID_SERVICE  the window is charged only when the requester changes, so
//                   its length is not W opportunities and the quota stops
//                   bounding what a class consumes: the waiting bound is no
//                   longer computable from the policy's own numbers.
// ============================================================================

`default_nettype none
`resetall

module mosaic_arbiter (
    input  logic                             clk,
    input  logic                             rst,

    // ------------------------------------------------------ request classes
    // One valid/ready port per arbitration class, class 0 in the low lane.
    // `req_src` is the requesting producer's identity: it travels with the
    // request and is delivered with the grant. It is the requester's tag, not
    // an identity this module compares.
    input  logic [3:0]                       req_valid,
    output logic [3:0]                       req_ready,
    input  logic [3:0][7:0]                  req_src,

    // --------------------------------------------------------- service port
    // One completion per opportunity. `srv_busy` is the downstream's
    // backpressure: a grant transfer is `srv_go && !srv_busy`.
    output logic                             srv_go,
    output logic [1:0]                       srv_class,
    output logic [7:0]                       srv_src,
    input  logic                             srv_busy,

    // -------------------------------------------------- window observation
    // The window's identity and the per-class grants inside it, so a consumer
    // can check the reservation from outside instead of trusting it.
    output logic [31:0]                      o_window_index,
    output logic [31:0]                      o_window_grants,
    output logic [3:0][31:0]                 o_used,
    output logic [3:0][31:0]                 o_pend_count,

    // -------------------------------------------------------- counters
    output logic [31:0]                      o_grant_ctr,      // completions delivered
    output logic [31:0]                      o_escape_ctr,     // grants over quota
    output logic [31:0]                      o_accept_ctr,     // requests admitted
    output logic [31:0]                      o_refuse_ctr,     // refused admissions (backpressure)
    output logic [31:0]                      o_stall_ctr,      // pending work, port not ready
    output logic [31:0]                      o_drop_ctr,       // must stay zero
    output logic [31:0]                      o_busy_run_max,   // longest !ready run with work
    output logic                             o_assumption_violated,

    // -------------------------------------------------- geometry read-back
    output logic [31:0]                      o_classes,
    output logic [31:0]                      o_window,
    output logic [31:0]                      o_d_max,
    output logic [31:0]                      o_depth,
    output logic [3:0][31:0]                 o_quota
);

  // --------------------------------------------------------------- geometry
  // localparam, not parameter: the policy is one geometry and a caller-supplied
  // second one would be a second policy. A driver reads these back on the o_*
  // ports rather than restating them.
  localparam int unsigned ARB_N_CLASSES = 4;
  localparam int unsigned ARB_DEPTH     = 4;   // entries per class queue
  localparam int unsigned ARB_SRCW      = 8;   // requester tag width
  localparam int unsigned ARB_TSW       = 32;  // arrival timestamp width
  localparam int unsigned ARB_QW        = 8;   // quota / used-counter width
  localparam int unsigned ARB_WINW      = 4;   // window grant counter width
  localparam int unsigned ARB_CNTW      = 3;   // $clog2(ARB_DEPTH + 1)
  localparam int unsigned ARB_CLSW      = 2;   // $clog2(ARB_N_CLASSES)

  // The quotas. Their sum is the window: every grant in a window is charged to
  // the granted class, so the window is exactly as long as the quotas are wide.
  localparam int unsigned ARB_Q_SCALAR = 4;
  localparam int unsigned ARB_Q_MEMORY = 2;
  localparam int unsigned ARB_Q_MULDIV = 1;
  localparam int unsigned ARB_Q_VECTOR = 1;
  localparam int unsigned ARB_WINDOW   = ARB_Q_SCALAR + ARB_Q_MEMORY
                                       + ARB_Q_MULDIV + ARB_Q_VECTOR;

  // The declared downstream assumption: the port is not ready for at most this
  // many consecutive cycles while work is pending. `ARB_WINDOW` opportunities
  // therefore take at most `ARB_WINDOW * ARB_D_MAX` cycles.
  localparam int unsigned ARB_D_MAX = 4;

  // The quotas as a vector, class 0 in the least significant slice, so the
  // selection loop can index them with a variable.
  localparam logic [ARB_N_CLASSES*ARB_QW-1:0] ARB_QUOTA =
      {ARB_QW'(ARB_Q_VECTOR), ARB_QW'(ARB_Q_MULDIV),
       ARB_QW'(ARB_Q_MEMORY), ARB_QW'(ARB_Q_SCALAR)};

  // --------------------------------------------------------------- the state
  // Queue storage. Validity lives entirely in `q_cnt`: an entry at or above the
  // count is data and nothing reads it, which is the rule mosaic_result_fifo
  // documents for its own storage.
  logic [ARB_DEPTH-1:0][ARB_SRCW-1:0] q_src_q [ARB_N_CLASSES];
  logic [ARB_DEPTH-1:0][ARB_TSW-1:0]  q_ts_q  [ARB_N_CLASSES];
  logic [ARB_CNTW-1:0]                q_cnt_q [ARB_N_CLASSES];
  // ...and the next-state form, computed combinationally and registered.
  logic [ARB_DEPTH-1:0][ARB_SRCW-1:0] q_src_d [ARB_N_CLASSES];
  logic [ARB_DEPTH-1:0][ARB_TSW-1:0]  q_ts_d  [ARB_N_CLASSES];
  logic [ARB_CNTW-1:0]                q_cnt_d [ARB_N_CLASSES];

  // The window's own state.
  logic [ARB_QW-1:0]   used_q [ARB_N_CLASSES];
  logic [ARB_WINW-1:0] win_grants_q;
  logic [31:0]         win_index_q;
  logic [ARB_TSW-1:0]  ts_q;

  // The assumption's state: the current run of pending-and-not-ready cycles and
  // the longest run ever seen.
  logic [31:0]  run_q;
  logic [31:0]  run_max_q;
  logic         viol_q;

`ifdef MOSAIC_ARB_MUTANT_REQ_ID_SERVICE
  // MUTANT: the previous grant's requester, so the mutant can charge the window
  // only when the requester changes.
  logic [ARB_SRCW-1:0] last_src_q;
`endif

  // ------------------------------------------------------------- combinational
  logic [ARB_N_CLASSES-1:0] pend;
  logic [ARB_N_CLASSES-1:0] elig;
  logic [ARB_N_CLASSES-1:0] pick;
  logic [ARB_N_CLASSES-1:0] push_ok;
  logic                     pend_any;
  logic                     elig_any;
  logic                     escape;
  logic                     opp;
  logic                     srv_go_t;
  logic                     pop_tx;
  logic                     charge;
  logic [ARB_CLSW-1:0]      sel;
  logic                     sel_found;
  logic [ARB_TSW-1:0]       sel_key;
  logic [ARB_TSW-1:0]       cand_key;
  logic [31:0]              run_next;
  logic [31:0]              refuse_now;
  logic [31:0]              accept_now;

  // A class queue is full at ARB_DEPTH; ready is a function of the registered
  // count alone (no combinational path from the downstream, no same-cycle slot
  // reuse for a same-cycle push -- see the header).
  always_comb begin
    push_ok   = 4'd0;
    pend      = 4'd0;
    refuse_now = 32'd0;
    accept_now = 32'd0;
    for (int unsigned c = 0; c < ARB_N_CLASSES; c = c + 1) begin
      req_ready[c] = (q_cnt_q[c] < ARB_CNTW'(ARB_DEPTH));
      pend[c]      = (q_cnt_q[c] != ARB_CNTW'(0));
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

  // The selection. `elig` is the quota-filtered set; when it is empty the whole
  // pending set is the escape set.
  always_comb begin
    elig     = 4'd0;
    elig_any = 1'b0;
    for (int unsigned c = 0; c < ARB_N_CLASSES; c = c + 1) begin
`ifdef MOSAIC_ARB_MUTANT_STARVE
      // MUTANT: the quota is not consulted, so every pending class is eligible.
      elig[c] = pend[c];
`else
      elig[c] = pend[c] && (used_q[c] < ARB_QUOTA[c*ARB_QW +: ARB_QW]);
`endif
      if (elig[c]) begin
        elig_any = 1'b1;
      end
    end

    escape = (elig_any == 1'b0) && pend_any;

    if (escape) begin
      pick = pend;
    end else begin
      pick = elig;
    end

    // The oldest pending request among the pick set wins; ties on the lowest
    // class index. The arrival timestamps are 32 bits and a run is far shorter
    // than half the modulus, so the unsigned comparison is exact (the plan's
    // section 1.3 modulus rule).
    sel_found = 1'b0;
    sel       = {ARB_CLSW{1'b0}};
    sel_key   = {ARB_TSW{1'b1}};
    for (int unsigned c = 0; c < ARB_N_CLASSES; c = c + 1) begin
`ifdef MOSAIC_ARB_MUTANT_STARVE
      // MUTANT: strict class priority instead of age.
      cand_key = ARB_TSW'(c);
`else
      cand_key = q_ts_q[c][0];
`endif
      if (pick[c] && ((sel_found == 1'b0) || (cand_key < sel_key))) begin
        sel       = ARB_CLSW'(c);
        sel_key   = cand_key;
        sel_found = 1'b1;
      end
    end
  end

  always_comb begin
    opp = pend_any && !srv_busy;
`ifdef MOSAIC_ARB_MUTANT_DROP_ON_QUOTA
    // MUTANT: on the escape path the selected request is removed from its queue
    // and reported as nothing. Loss instead of backpressure.
    srv_go_t = opp && !escape;
    pop_tx   = opp;
`else
    srv_go_t = opp;
    pop_tx   = opp;
`endif

    // A grant is charged to the window on every transfer...
`ifdef MOSAIC_ARB_MUTANT_REQ_ID_SERVICE
    // MUTANT: ...except when the requester is the one that was just served, so
    // a run of same-requester grants consumes one quota slot and the window's
    // length in opportunities is not W any more.
    charge = pop_tx && (req_src[sel] != last_src_q);
`else
    charge = pop_tx;
`endif

    run_next = (pend_any && srv_busy) ? (run_q + 32'd1) : 32'd0;
  end

  assign srv_go    = srv_go_t;
  assign srv_class = sel;
  assign srv_src   = q_src_q[sel][0];

  // -------------------------------------------------------------- next state
  // Each class queue is rebuilt from its survivors (minus the granted head) and
  // then the newly accepted request, so a same-cycle pop and push cannot alias
  // and a refusal is never a loss.
  always_comb begin
    int unsigned wr;
    for (int unsigned c = 0; c < ARB_N_CLASSES; c = c + 1) begin
      wr = 0;
      for (int unsigned k = 0; k < ARB_DEPTH; k = k + 1) begin
        if (k < q_cnt_q[c]) begin
          if (!(pop_tx && (sel == ARB_CLSW'(c)) && (k == 0))) begin
            q_src_d[c][wr] = q_src_q[c][k];
            q_ts_d[c][wr]  = q_ts_q[c][k];
            wr = wr + 1;
          end
        end
      end
      if (push_ok[c]) begin
        q_src_d[c][wr] = req_src[c];
        q_ts_d[c][wr]  = ts_q;
        wr = wr + 1;
      end
      // The entries at or above the new count carry no meaning; they are
      // cleared so the whole vector is assigned on every path (a partially
      // assigned combinational array is a latch to a linter, and a stale entry
      // is a surprise to whoever reads the observation port).
      for (int unsigned k = wr; k < ARB_DEPTH; k = k + 1) begin
        q_src_d[c][k] = {ARB_SRCW{1'b0}};
        q_ts_d[c][k]  = {ARB_TSW{1'b0}};
      end
      q_cnt_d[c] = ARB_CNTW'(wr);
    end
  end

  // ----------------------------------------------------------------- the edge
  always_ff @(posedge clk) begin
    if (rst) begin
      ts_q         <= {ARB_TSW{1'b0}};
      win_grants_q <= {ARB_WINW{1'b0}};
      win_index_q  <= 32'd0;
      run_q        <= 32'd0;
      run_max_q    <= 32'd0;
      viol_q       <= 1'b0;
      o_grant_ctr  <= 32'd0;
      o_escape_ctr <= 32'd0;
      o_accept_ctr <= 32'd0;
      o_refuse_ctr <= 32'd0;
      o_stall_ctr  <= 32'd0;
      o_drop_ctr   <= 32'd0;
      for (int unsigned c = 0; c < ARB_N_CLASSES; c = c + 1) begin
        q_cnt_q[c] <= ARB_CNTW'(0);
        used_q[c]  <= {ARB_QW{1'b0}};
      end
`ifdef MOSAIC_ARB_MUTANT_REQ_ID_SERVICE
      last_src_q <= {ARB_SRCW{1'b0}};
`endif
    end else begin
      ts_q <= ts_q + ARB_TSW'(1);

      // The queues.
      for (int unsigned c = 0; c < ARB_N_CLASSES; c = c + 1) begin
        q_cnt_q[c] <= q_cnt_d[c];
        for (int unsigned k = 0; k < ARB_DEPTH; k = k + 1) begin
          q_src_q[c][k] <= q_src_d[c][k];
          q_ts_q[c][k]  <= q_ts_d[c][k];
        end
      end

      // The window. A charged grant consumes one opportunity; the W-th grant
      // closes the window and opens the next one.
      if (charge) begin
        if (win_grants_q == ARB_WINW'(ARB_WINDOW - 1)) begin
          win_grants_q <= {ARB_WINW{1'b0}};
          win_index_q  <= win_index_q + 32'd1;
          for (int unsigned c = 0; c < ARB_N_CLASSES; c = c + 1) begin
            used_q[c] <= {ARB_QW{1'b0}};
          end
        end else begin
          win_grants_q <= win_grants_q + ARB_WINW'(1);
          used_q[sel]  <= used_q[sel] + ARB_QW'(1);
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

      // The assumption. `run_next` is this cycle's run length; the flag is
      // sticky until reset, because it invalidates the bound for the stretch it
      // was observed in and a case must be able to see it after the fact.
      run_q <= run_next;
      if (run_next > run_max_q) begin
        run_max_q <= run_next;
      end
      if (run_next > 32'(ARB_D_MAX)) begin
        viol_q <= 1'b1;
      end

`ifdef MOSAIC_ARB_MUTANT_REQ_ID_SERVICE
      if (pop_tx) begin
        last_src_q <= req_src[sel];
      end
`endif
    end
  end

  // ------------------------------------------------------------- observation
  assign o_window_index  = win_index_q;
  assign o_window_grants = 32'(win_grants_q);
  assign o_busy_run_max  = run_max_q;
  assign o_assumption_violated = viol_q;
  assign o_classes = 32'(ARB_N_CLASSES);
  assign o_window  = 32'(ARB_WINDOW);
  assign o_d_max   = 32'(ARB_D_MAX);
  assign o_depth   = 32'(ARB_DEPTH);

  for (genvar c = 0; c < ARB_N_CLASSES; c = c + 1) begin : g_observe
    assign o_used[c]       = 32'(used_q[c]);
    assign o_pend_count[c] = 32'(q_cnt_q[c]);
    assign o_quota[c]      = 32'(ARB_QUOTA[c*ARB_QW +: ARB_QW]);
  end

endmodule : mosaic_arbiter

`default_nettype wire
