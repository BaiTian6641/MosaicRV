// ============================================================================
// mosaic_mshr -- a non-blocking L1 read path with an outstanding-miss table.
// Work package I-043. CASE=cache.mshr_nonblocking.
//
// The blocking cache (I-042, rtl/core/mosaic_cache.sv) accepts one CPU request
// and finishes it before the next one starts: a miss costs the whole cache. This
// module keeps the same storage geometry but decouples the CPU side from the
// memory side with an MSHR (miss-status holding register) table, so several
// misses can be outstanding at once and a request that hits never waits behind
// one that does not.
//
// ------------------------------------------------------------ what it is not
//
// This is the *read* path: loads and instruction fetches. There is no store
// port, no dirty bit and no writeback, so a line is always clean and may be
// dropped on eviction without a memory transaction. Write-allocate stores over
// an MSHR are deliberately not implemented (I-042 owns the store/writeback
// policy; see results/reports/I-043-mshr.md, "not covered").
//
// ------------------------------------------------------------- the rules
//
// R1  A request that hits is answered from the array in one cycle. It does not
//     touch the MSHR and it does not wait for any outstanding miss.
//
// R2  A request that misses allocates an MSHR entry and is accepted. If the
//     line is already the subject of a live entry, the request *coalesces*: it
//     joins that entry as another waiter and NO second memory read is issued.
//     One line therefore has at most one outstanding read, and every waiter on
//     that line is answered from the one response.
//
// R3  A request that misses and finds every entry occupied is not accepted
//     (`req_ready` low). Back-pressure, never a dropped miss.
//
// R4  Each waiter remembers the word it asked for, so coalesced requests to
//     different words of one line each get their own word back.
//
// R5  A response is matched to an entry by the line address it carries, not by
//     arrival order, so responses for different lines may return in any order.
//     Every waiter is answered exactly once; a waiter never gets a second
//     response and a request that was never accepted never gets one at all.
//
// R6  A request may be cancelled (`cancel_valid`/`cancel_id`) after it was
//     accepted and while its miss is outstanding -- the squashed-load case.
//     Cancelling one waiter never affects another waiter on the same line or a
//     different entry.
//
// R7  A cancelled request's refill response is absorbed: it is NOT installed as
//     valid (the access that would have justified the line is gone) and the
//     entry is freed, so the table never leaks an entry on a cancellation. A
//     refill that was *not* cancelled installs the line, which is how the next
//     request to it becomes a hit.
//
// R8  An entry cancelled before its read was issued is freed without ever
//     issuing a memory request.
//
// R9  Conservation: every accepted miss request becomes exactly one of a
//     delivered response, an applied cancellation, or a live waiter. The
//     testbench checks `miss_accepted == responses + cancels + dbg_waiters`
//     every cycle; an entry that silently vanishes, or is answered twice, or
//     leaks, breaks it. `dbg_waiters` is the sum of the waiter bits and
//     `dbg_outstanding` counts live entries.
//
// ---------------------------------------------------------- response channel
//
// One response per cycle, no back-pressure: a hit response and a drained MSHR
// waiter can never share a cycle, because a hit accepted in cycle T is held in a
// one-deep register and delivered in T+1 while a drain uses the port in T.
//
// ------------------------------------------------------------- memory port
//
// One line request channel and one line response channel:
//
//   * request: `mem_req_valid` / `mem_req_addr` (line aligned), accepted on an
//     edge where `mem_req_valid && mem_req_ready`. `mem_req_ready` may be low
//     for any number of cycles; the entry holds its request until accepted.
//   * response: `mem_resp_valid` / `mem_resp_addr` (line aligned) /
//     `mem_resp_rdata` / `mem_resp_fault`. The address identifies the line, so
//     responses may arrive in any order and a response is never assumed to be
//     the oldest outstanding one. A response that matches no live entry is
//     ignored (it cannot happen in the shipping design; it is what a duplicate
//     read would produce, so ignoring it keeps a mutant from hanging).
//
// ------------------------------------------------------------------ mutants
//
// -DMOSAIC_MSHR_MUTANT_* selects a deliberately broken variant used to prove
// the unit test detects the corresponding bug. The shipping build defines none
// of them. See results/reports/I-043-mshr.md for the control table.
// ============================================================================

`default_nettype none
`resetall

module mosaic_mshr #(
  parameter int CPU_DATA_WIDTH = 64,    // bits per CPU access (a word)
  parameter int LINE_BYTES     = 32,    // bytes per line, power of two
  parameter int SETS           = 8,     // sets, direct-mapped
  parameter int ADDR_WIDTH     = 32,    // physical address width
  parameter int MSHR_ENTRIES   = 4,     // outstanding-miss entries
  parameter int ID_WIDTH       = 3,     // request id bits (1 << ID_WIDTH ids)
  // Derived, and therefore not overridable.
  localparam int CPU_BYTES     = CPU_DATA_WIDTH / 8,
  localparam int LINE_BITS     = LINE_BYTES * 8,
  localparam int OFFSET_BITS   = $clog2(LINE_BYTES),
  localparam int INDEX_BITS    = $clog2(SETS),
  localparam int TAG_WIDTH     = ADDR_WIDTH - OFFSET_BITS - INDEX_BITS,
  localparam int WORD_BITS     = $clog2(CPU_BYTES),          // byte-in-word bits
  localparam int WORD_IDX_BITS = OFFSET_BITS - WORD_BITS,    // word-in-line bits
  localparam int IDS           = 1 << ID_WIDTH,
  localparam int ENT_IDX_BITS  = (MSHR_ENTRIES > 1) ? $clog2(MSHR_ENTRIES) : 1,
  localparam int ENT_CNT_BITS  = $clog2(MSHR_ENTRIES + 1),
  localparam int WT_CNT_BITS   = $clog2(MSHR_ENTRIES * IDS + 1)
) (
  input  logic                        clk,
  input  logic                        rst,

  // ------------------------------------------------------------ CPU request
  input  logic                        req_valid,
  /* verilator lint_off UNUSEDSIGNAL */
  input  logic [ADDR_WIDTH-1:0]       req_addr,   // byte address, word aligned
  /* verilator lint_on UNUSEDSIGNAL */
  input  logic [ID_WIDTH-1:0]         req_id,
  output logic                        req_ready,

  // --------------------------------------------------- cancellation of one id
  input  logic                        cancel_valid,
  input  logic [ID_WIDTH-1:0]         cancel_id,

  // ----------------------------------------------------------- CPU response
  output logic                        resp_valid,
  output logic [ID_WIDTH-1:0]         resp_id,
  output logic [CPU_DATA_WIDTH-1:0]   resp_rdata,
  output logic                        resp_fault,

  // ----------------------------------------------------------- memory port
  output logic                        mem_req_valid,
  output logic [ADDR_WIDTH-1:0]       mem_req_addr,   // line aligned
  input  logic                        mem_req_ready,
  input  logic                        mem_resp_valid,
  /* verilator lint_off UNUSEDSIGNAL */
  input  logic [ADDR_WIDTH-1:0]       mem_resp_addr,  // line aligned
  /* verilator lint_on UNUSEDSIGNAL */
  input  logic [LINE_BITS-1:0]        mem_resp_rdata,
  input  logic                        mem_resp_fault,

  // -------------------------------------------------------- state inspection
  input  logic [INDEX_BITS-1:0]       dbg_index,
  output logic                        dbg_valid,
  output logic [TAG_WIDTH-1:0]        dbg_tag,
  output logic [LINE_BITS-1:0]        dbg_data,
  output logic [ENT_CNT_BITS-1:0]     dbg_outstanding,
  output logic [WT_CNT_BITS-1:0]      dbg_waiters,

  // ------------------------------------------------------------ trace events
  output logic                        ev_hit,
  output logic                        ev_miss,
  output logic                        ev_coalesce,
  output logic                        ev_refill,
  output logic                        ev_fault,
  output logic                        ev_cancel,
  output logic                        ev_drop
);

  //  ------------------------------------------------------------------ storage
  // Same contract as I-042: no reset and no initial value on the arrays; the
  // valid bit is the only per-line control state and it IS cleared by `rst`.
  logic [LINE_BITS-1:0] data_mem [SETS];
  logic [TAG_WIDTH-1:0] tag_mem  [SETS];
  logic [SETS-1:0]      valid;

  // ------------------------------------------------------------------ MSHR
  logic                  e_valid   [MSHR_ENTRIES];
  logic [ADDR_WIDTH-1:0] e_line    [MSHR_ENTRIES];   // line-aligned address
  logic                  e_issued  [MSHR_ENTRIES];   // read handed to memory
  logic                  e_ready   [MSHR_ENTRIES];   // good response held
  logic                  e_fault   [MSHR_ENTRIES];   // response was a fault
  logic                  e_served  [MSHR_ENTRIES];   // answered >=1 waiter
  logic [IDS-1:0]        e_waiters [MSHR_ENTRIES];
  logic [WORD_IDX_BITS-1:0] e_word [MSHR_ENTRIES][IDS];  // word per waiter
  logic [LINE_BITS-1:0]  e_data    [MSHR_ENTRIES];   // held line after response

  // One-deep hit response register (see "response channel" above).
  logic                    hit_resp_valid_r;
  logic [ID_WIDTH-1:0]     hit_resp_id_r;
  logic [CPU_DATA_WIDTH-1:0] hit_resp_data_r;

  // Elaboration guards: a geometry this module cannot represent is an
  // elaboration error, not a silent misbuild.
  generate
    if ((1 << OFFSET_BITS) != LINE_BYTES) begin : g_bad_line_bytes
      mosaic_mshr_contract_violation u_line_bytes ();
    end
    if ((1 << INDEX_BITS) != SETS) begin : g_bad_sets
      mosaic_mshr_contract_violation u_sets ();
    end
    if ((TAG_WIDTH < 1) || ((1 << WORD_BITS) != CPU_BYTES)) begin : g_bad_tag
      mosaic_mshr_contract_violation u_tag ();
    end
    if ((MSHR_ENTRIES < 1) || (IDS < 2)) begin : g_bad_entries
      mosaic_mshr_contract_violation u_entries ();
    end
  endgenerate

  // --------------------------------------------------------- request decode
  logic [INDEX_BITS-1:0] req_index;
  logic [TAG_WIDTH-1:0]  req_tag;
  logic [WORD_IDX_BITS-1:0] req_word;
  logic [ADDR_WIDTH-1:0] req_line;
  assign req_index = req_addr[OFFSET_BITS + INDEX_BITS - 1 : OFFSET_BITS];
  assign req_tag   = req_addr[ADDR_WIDTH - 1 : OFFSET_BITS + INDEX_BITS];
  assign req_word  = req_addr[OFFSET_BITS - 1 : WORD_BITS];
  assign req_line  = {req_addr[ADDR_WIDTH - 1 : OFFSET_BITS], {OFFSET_BITS{1'b0}}};

  logic req_hit;
  assign req_hit = valid[req_index] && (tag_mem[req_index] == req_tag);

  // ---------------------------------------------------------- MSHR lookups
  logic                    dup_found;
  logic [ENT_IDX_BITS-1:0] dup_idx;
  logic                    free_found;
  logic [ENT_IDX_BITS-1:0] free_idx;
  always_comb begin
    dup_found  = 1'b0;
    dup_idx    = '0;
    free_found = 1'b0;
    free_idx   = '0;
    for (int unsigned i = 0; i < MSHR_ENTRIES; i++) begin
      if (!dup_found && e_valid[i] && (e_line[i] == req_line)) begin
        dup_found = 1'b1;
        dup_idx   = i[ENT_IDX_BITS-1:0];
      end
      if (!free_found && !e_valid[i]) begin
        free_found = 1'b1;
        free_idx   = i[ENT_IDX_BITS-1:0];
      end
    end
  end

`ifdef MOSAIC_MSHR_MUTANT_NO_COALESCE
  // MUTANT 1: a duplicate miss is not coalesced. A second request to a line
  // already outstanding allocates a second entry and issues a second read of
  // the same line, so one line has two reads in flight.
  logic dup_use;
  assign dup_use = 1'b0;
`else
  logic dup_use;
  assign dup_use = dup_found;
`endif

  assign req_ready = req_hit || dup_use || free_found;

  // ------------------------------------------------------------ memory issue
  logic                    issue_valid;
  logic [ADDR_WIDTH-1:0]   issue_addr;
  logic [ENT_IDX_BITS-1:0] issue_idx;
  always_comb begin
    issue_valid = 1'b0;
    issue_addr  = '0;
    issue_idx   = '0;
    for (int unsigned i = 0; i < MSHR_ENTRIES; i++) begin
      if (!issue_valid && e_valid[i] && !e_issued[i] && !e_ready[i] && !e_fault[i] &&
          (e_waiters[i] != {IDS{1'b0}})) begin
        issue_valid = 1'b1;
        issue_addr  = e_line[i];
        issue_idx   = i[ENT_IDX_BITS-1:0];
      end
    end
  end
  assign mem_req_valid = issue_valid;
  assign mem_req_addr  = issue_addr;

  // -------------------------------------------------------- response matching
  logic [ADDR_WIDTH-1:0] mem_resp_line;
  assign mem_resp_line = {mem_resp_addr[ADDR_WIDTH - 1 : OFFSET_BITS],
                          {OFFSET_BITS{1'b0}}};

  logic [MSHR_ENTRIES-1:0] resp_match;
  always_comb begin
    for (int unsigned i = 0; i < MSHR_ENTRIES; i++) begin
      resp_match[i] = e_valid[i] && e_issued[i] && !e_ready[i] && !e_fault[i] &&
                      (e_line[i] == mem_resp_line);
    end
  end

  // ------------------------------------------------------------- drain select
  logic                    drain_valid;
  logic [ENT_IDX_BITS-1:0] drain_idx;
  logic [ID_WIDTH-1:0]     drain_id;
  always_comb begin
    drain_valid = 1'b0;
    drain_idx   = '0;
    for (int unsigned i = 0; i < MSHR_ENTRIES; i++) begin
      if (!drain_valid && e_valid[i] && (e_ready[i] || e_fault[i]) &&
          (e_waiters[i] != {IDS{1'b0}})) begin
        drain_valid = 1'b1;
        drain_idx   = i[ENT_IDX_BITS-1:0];
      end
    end
  end
  always_comb begin
    drain_id = '0;
    for (int unsigned k = 0; k < IDS; k++) begin
      if (e_waiters[drain_idx][k]) begin
        drain_id = k[ID_WIDTH-1:0];
        break;
      end
    end
  end

  // ---------------------------------------------------------------- responses
  logic resp_from_hit;
  assign resp_from_hit = hit_resp_valid_r;
  assign resp_valid    = resp_from_hit || drain_valid;
  assign resp_fault    = resp_from_hit ? 1'b0 : e_fault[drain_idx];
  assign resp_rdata    = resp_from_hit
                           ? hit_resp_data_r
                           : (e_fault[drain_idx]
                                ? {CPU_DATA_WIDTH{1'b0}}
                                : e_data[drain_idx][int'(e_word[drain_idx][drain_id]) *
                                                    CPU_DATA_WIDTH +: CPU_DATA_WIDTH]);
`ifdef MOSAIC_MSHR_MUTANT_WRONG_ID
  // MUTANT 2: a coalesced response carries the wrong requester id -- the id is
  // perturbed while the data still belongs to the real waiter. The identity
  // rule ("this response answers that request") is what this breaks.
  assign resp_id = resp_from_hit ? hit_resp_id_r
                                 : (drain_id ^ {{(ID_WIDTH-1){1'b0}}, 1'b1});
`else
  assign resp_id = resp_from_hit ? hit_resp_id_r : drain_id;
`endif

  // ---------------------------------------------------------------- cancel
  logic cancel_hit_any;
  always_comb begin
    cancel_hit_any = 1'b0;
    for (int unsigned i = 0; i < MSHR_ENTRIES; i++) begin
      // A waiter that the drain is answering this cycle is not "cancelled":
      // the response removes it, and reporting both would double-count it.
      if (e_valid[i] && e_waiters[i][cancel_id] &&
          !(drain_valid && !hit_resp_valid_r &&
            (drain_idx == i[ENT_IDX_BITS-1:0]) && (drain_id == cancel_id))) begin
        cancel_hit_any = 1'b1;
      end
    end
  end
`ifdef MOSAIC_MSHR_MUTANT_CANCEL_IGNORED
  // MUTANT 3: the cancellation is dropped. The squashed request keeps its
  // waiter bit, so its response is delivered after all, and (because the entry
  // is treated as having served a waiter) its refill is installed as valid.
  localparam bit CANCEL_APPLY = 1'b0;
`else
  localparam bit CANCEL_APPLY = 1'b1;
`endif

  // ------------------------------------------------------------ debug view
  assign dbg_valid = valid[dbg_index];
  assign dbg_tag   = tag_mem[dbg_index];
  assign dbg_data  = data_mem[dbg_index];

  logic [ENT_CNT_BITS-1:0] outstanding_count;
  logic [ENT_CNT_BITS:0]   outstanding_sum;
  always_comb begin
    outstanding_sum = '0;
    for (int unsigned i = 0; i < MSHR_ENTRIES; i++) begin
      if (e_valid[i]) outstanding_sum = outstanding_sum + {{ENT_CNT_BITS{1'b0}}, 1'b1};
    end
    outstanding_count = outstanding_sum[ENT_CNT_BITS-1:0];
  end
  assign dbg_outstanding = outstanding_count;

  logic [WT_CNT_BITS-1:0] waiter_count;
  logic [WT_CNT_BITS:0]   waiter_sum;
  always_comb begin
    waiter_sum = '0;
    for (int unsigned i = 0; i < MSHR_ENTRIES; i++) begin
      for (int unsigned k = 0; k < IDS; k++) begin
        if (e_waiters[i][k]) waiter_sum = waiter_sum + {{WT_CNT_BITS{1'b0}}, 1'b1};
      end
    end
    waiter_count = waiter_sum[WT_CNT_BITS-1:0];
  end
  assign dbg_waiters = waiter_count;

  // ------------------------------------------------------------- next state
  // All per-entry next values are computed combinationally, then registered in
  // one place, so the precedence between "allocate", "issue", "respond",
  // "drain", "cancel" and "complete" is written down once instead of being an
  // accident of statement order inside an always_ff.
  logic                    nx_valid   [MSHR_ENTRIES];
  logic [ADDR_WIDTH-1:0]   nx_line    [MSHR_ENTRIES];
  logic                    nx_issued  [MSHR_ENTRIES];
  logic                    nx_ready   [MSHR_ENTRIES];
  logic                    nx_fault   [MSHR_ENTRIES];
  logic                    nx_served  [MSHR_ENTRIES];
  logic [IDS-1:0]          nx_waiters [MSHR_ENTRIES];
  logic [LINE_BITS-1:0]    nx_data    [MSHR_ENTRIES];

  logic                    install_en;
  logic [ENT_IDX_BITS-1:0] install_idx;
  logic [INDEX_BITS-1:0]   install_set;
  assign install_set = nx_line[install_idx][OFFSET_BITS+INDEX_BITS-1:OFFSET_BITS];

  logic [IDS-1:0] req_id_bit;
  logic [IDS-1:0] cancel_id_bit;
  assign req_id_bit    = {{(IDS-1){1'b0}}, 1'b1} << req_id;
  assign cancel_id_bit = {{(IDS-1){1'b0}}, 1'b1} << cancel_id;

  always_comb begin
    for (int unsigned i = 0; i < MSHR_ENTRIES; i++) begin
      nx_valid[i]   = e_valid[i];
      nx_line[i]    = e_line[i];
      nx_issued[i]  = e_issued[i];
      nx_ready[i]   = e_ready[i];
      nx_fault[i]   = e_fault[i];
      nx_served[i]  = e_served[i];
      nx_waiters[i] = e_waiters[i];
      nx_data[i]    = e_data[i];
    end

    // R1/R2/R3 -- accept a request: hit (no MSHR), coalesce, or allocate.
    if (req_valid && req_ready) begin
      if (!req_hit) begin
        if (dup_use) begin
          nx_waiters[dup_idx] = nx_waiters[dup_idx] | req_id_bit;
        end else if (free_found) begin
          nx_valid[free_idx]   = 1'b1;
          nx_line[free_idx]    = req_line;
          nx_issued[free_idx]  = 1'b0;
          nx_ready[free_idx]   = 1'b0;
          nx_fault[free_idx]   = 1'b0;
          nx_served[free_idx]  = 1'b0;
          nx_waiters[free_idx] = req_id_bit;
          nx_data[free_idx]    = {LINE_BITS{1'b0}};
        end
      end
    end

    // Memory request accepted.
    if (issue_valid && mem_req_ready) begin
      nx_issued[issue_idx] = 1'b1;
    end

    // R5 -- match the response by line address; every matching entry takes it.
    if (mem_resp_valid) begin
      for (int unsigned i = 0; i < MSHR_ENTRIES; i++) begin
        if (resp_match[i]) begin
          if (mem_resp_fault) begin
            nx_fault[i] = 1'b1;
          end else begin
            nx_ready[i] = 1'b1;
            nx_data[i]  = mem_resp_rdata;
          end
        end
      end
    end

    // R4 -- answer one waiter of the selected ready/fault entry.
    if (drain_valid && !hit_resp_valid_r) begin
      nx_waiters[drain_idx] = nx_waiters[drain_idx] & ~({{(IDS-1){1'b0}}, 1'b1} << drain_id);
      nx_served[drain_idx]  = 1'b1;
    end

    // R6 -- drop one waiter per cancelled id.
    if (cancel_valid && cancel_hit_any && CANCEL_APPLY) begin
      for (int unsigned i = 0; i < MSHR_ENTRIES; i++) begin
        if (nx_valid[i] && nx_waiters[i][cancel_id]) begin
          nx_waiters[i] = nx_waiters[i] & ~cancel_id_bit;
        end
      end
    end

    // R7/R8 -- select at most one completed entry to install, then free the
    // entries that are done. An entry whose every waiter was cancelled is
    // dropped without installing; an entry that answered a waiter installs.
    install_en  = 1'b0;
    install_idx = '0;
    for (int unsigned i = 0; i < MSHR_ENTRIES; i++) begin
      if (!install_en && nx_valid[i] && nx_ready[i] &&
          (nx_waiters[i] == {IDS{1'b0}})) begin
        install_en  = 1'b1;
        install_idx = i[ENT_IDX_BITS-1:0];
      end
    end
    for (int unsigned i = 0; i < MSHR_ENTRIES; i++) begin
      if (nx_valid[i] && (nx_waiters[i] == {IDS{1'b0}})) begin
        if (nx_ready[i]) begin
          // The selected entry is installed if it answered a waiter and
          // absorbed (dropped) if every waiter was cancelled; either way it
          // frees here. A ready entry not selected this cycle waits for the
          // next one, so at most one install happens per cycle.
          if (install_en && (install_idx == i[ENT_IDX_BITS-1:0])) nx_valid[i] = 1'b0;
        end else if (nx_fault[i]) begin
          nx_valid[i] = 1'b0;
        end else if (!nx_issued[i]) begin
          nx_valid[i] = 1'b0;     // cancelled before its read was issued
        end
      end
    end
  end

  // The install the next-state logic selected. A cancelled refill selects no
  // install, so `install_en` already encodes R7.
  logic install_now;
  logic drop_now;
  assign install_now = install_en && nx_served[install_idx];
  assign drop_now    = install_en && !nx_served[install_idx];

  // ------------------------------------------------------------------- FSM
  always_ff @(posedge clk) begin
    ev_hit      <= 1'b0;
    ev_miss     <= 1'b0;
    ev_coalesce <= 1'b0;
    ev_refill   <= 1'b0;
    ev_fault    <= 1'b0;
    ev_cancel   <= 1'b0;
    ev_drop     <= 1'b0;

    if (rst) begin
      valid           <= {SETS{1'b0}};
      hit_resp_valid_r <= 1'b0;
      hit_resp_id_r    <= '0;
      hit_resp_data_r  <= '0;
      for (int unsigned i = 0; i < MSHR_ENTRIES; i++) begin
        e_valid[i]   <= 1'b0;
        e_line[i]    <= '0;
        e_issued[i]  <= 1'b0;
        e_ready[i]   <= 1'b0;
        e_fault[i]   <= 1'b0;
        e_served[i]  <= 1'b0;
        e_waiters[i] <= {IDS{1'b0}};
        e_data[i]    <= {LINE_BITS{1'b0}};
        for (int unsigned k = 0; k < IDS; k++) begin
          e_word[i][k] <= '0;
        end
      end
      // data_mem and tag_mem are deliberately NOT reset (see header).
    end else begin
      for (int unsigned i = 0; i < MSHR_ENTRIES; i++) begin
        e_valid[i]   <= nx_valid[i];
        e_line[i]    <= nx_line[i];
        e_issued[i]  <= nx_issued[i];
        e_ready[i]   <= nx_ready[i];
        e_fault[i]   <= nx_fault[i];
        e_served[i]  <= nx_served[i];
        e_waiters[i] <= nx_waiters[i];
        e_data[i]    <= nx_data[i];
      end

      // Remember the word each waiter asked for (R4).
      if (req_valid && req_ready && !req_hit) begin
        if (dup_use) begin
          e_word[dup_idx][req_id] <= req_word;
        end else if (free_found) begin
          e_word[free_idx][req_id] <= req_word;
        end
      end

      // Install the line the next-state logic selected.
      if (install_now) begin
        valid[install_set]    <= 1'b1;
        tag_mem[install_set]  <= nx_line[install_idx][ADDR_WIDTH-1:OFFSET_BITS+INDEX_BITS];
        data_mem[install_set] <= nx_data[install_idx];
        ev_refill             <= 1'b1;
      end
      if (drop_now) ev_drop <= 1'b1;   // a cancelled refill absorbed, not installed

      // Hit response register (one deep; see "response channel").
      if (req_valid && req_ready && req_hit) begin
        hit_resp_valid_r <= 1'b1;
        hit_resp_id_r    <= req_id;
        hit_resp_data_r  <= data_mem[req_index][int'(req_word) * CPU_DATA_WIDTH +: CPU_DATA_WIDTH];
      end else if (hit_resp_valid_r) begin
        hit_resp_valid_r <= 1'b0;
      end

      // Events.
      if (req_valid && req_ready && req_hit) begin
        ev_hit <= 1'b1;
      end
      if (req_valid && req_ready && !req_hit) begin
        ev_miss <= 1'b1;
        if (dup_use) ev_coalesce <= 1'b1;
      end
      if (cancel_valid && cancel_hit_any && CANCEL_APPLY) ev_cancel <= 1'b1;
      if (mem_resp_valid && mem_resp_fault) begin
        for (int unsigned i = 0; i < MSHR_ENTRIES; i++) begin
          if (resp_match[i]) ev_fault <= 1'b1;
        end
      end
    end
  end

endmodule

`resetall
