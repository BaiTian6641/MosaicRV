// ============================================================================
// mosaic_cache -- the non-blocking, set-associative, config-driven L1
//
// This started life as I-042's small *blocking* direct-mapped cache ("accepts
// exactly one CPU request and completes it before it can accept the next one")
// and is now the machine the plan's memory step asks for: the geometry is the
// profile's (`line_bytes`, `sets`, `ways` -- see `caches` in
// config/geometry/<profile>.json, emitted into mosaic_cfg_pkg), and the read
// path is non-blocking, so a request that hits completes while a miss is in
// flight and a second request to a line already outstanding *coalesces* onto
// the entry that is already fetching it instead of issuing a second read.
//
// The same module is both the L1I and the L1D. `READ_ONLY` selects which:
//
//   * READ_ONLY = 0  (L1D)  stores are legal and the write policy below applies;
//   * READ_ONLY = 1  (L1I)  there is no store path through an instruction cache
//                           in the p0 core, and a write request is answered with
//                           a fault and changes nothing.
//
// ------------------------------------------------- what "non-blocking" means
//
// There is one memory-side port and it carries one transaction at a time (that
// is the interface the cache is given; the width adapter and the core's port
// are single-outstanding too). "Non-blocking" therefore does *not* mean several
// refills racing on the wire. It means the CPU side is decoupled from the
// memory side by an outstanding-miss table:
//
//   * a CPU request that HITS is answered from the array in the next cycle and
//     never waits for any miss, even while the table is full;
//   * a CPU request that MISSES a line the table already holds JOINS that
//     entry -- no second memory read, one response per waiter;
//   * a CPU request that misses and finds the table full is not accepted
//     (`cpu_req_ready` low): back-pressure, never a dropped miss.
//
// The rules the table keeps are the ones I-043's `mosaic_mshr` states and its
// case proves, restated for a write-back cache: every waiter is answered
// exactly once, a faulted refill is never installed, and an entry is freed
// exactly when its last waiter has been answered. The conservation identity
// `accepted misses == responses + live waiters` therefore holds by construction.
//
// The L1I side of the core is served by `mosaic_mshr` directly (the same rules,
// id-tagged, clean read-only storage); this module is the L1D because a data
// cache needs the store/writeback policy below, which a clean read-only table
// cannot carry.
//
// ------------------------------------------------------------- write policy
//
// *** WRITE-BACK, WRITE-ALLOCATE. *** This is a rule, not an emergent
// behaviour, and every clause of it is exercised by CASE=cache.refill_evict_fault:
//
//   1. A store that hits its line merges the byte lanes selected by `wmask`
//      into the line and marks the line dirty. It does NOT go to memory.
//   2. A store that misses allocates an entry (or joins one already fetching
//      the line), is refilled from memory first (write-allocate), then the store
//      bytes are merged and the line is marked dirty before its response.
//   3. A dirty line is written back exactly when it is evicted by a refill that
//      needs its way, or by an explicit flush. A clean line is simply dropped.
//   4. A read-only cache never marks a line dirty; its flush only drops tags.
//
// ------------------------------------------------- valid bits vs. data RAM
//
// `rst` clears exactly `valid`, `dirty`, the victim pointers, the miss table and
// the trace pulses. It does NOT touch `data_mem` or `tag_mem`. Validity is
// tracked outside the arrays, so a `SETS x WAYS x (LINE_BYTES*8)` reset -- which
// would turn an inferrable block RAM into reset flops -- is neither needed nor
// present. After a reset every line reads back invalid, and the contents of
// `data_mem` are never observed until a refill has installed the line.
//
// -------------------------------------------------------- refill error handling
//
// The memory port can fail a refill (`mem_resp_fault`). On a failed refill the
// entry is marked faulted, the line is NOT installed, and every waiter on that
// entry is answered with `cpu_resp_fault` (no data). A subsequent request to
// the line therefore misses again and tries the refill afresh.
//
// -------------------------------------------------------------- traceability
//
//   ev_hit        a CPU request completed out of the cache;
//   ev_miss       a CPU request did not find its line (coalesced or not);
//   ev_coalesce   a miss joined an entry already fetching the same line;
//   ev_refill     a line was successfully fetched and installed;
//   ev_writeback  a dirty line was written back on eviction/flush;
//   ev_fault      a refill faulted.
//
// A line is only ever installed (`ev_refill`) after a *successful* refill, and
// `ev_writeback` only ever carries the line the cache held.
//
// ------------------------------------------------------------- memory port
//
// One request/response channel for whole lines:
//
//   * request: `mem_req_valid` / `mem_req_we` / `mem_req_addr` (line aligned) /
//     `mem_req_wdata`; accepted on an edge where `mem_req_valid && mem_req_ready`.
//     A writeback is complete when accepted. A read's data arrives later as
//     `mem_resp_valid` with `mem_resp_rdata` / `mem_resp_fault`. Reads are
//     issued one at a time: a new refill is not offered while a response is
//     still outstanding, which is what the single-outstanding port requires.
//   * `mem_req_ready` may be low for an unbounded number of cycles; the entry
//     holds its request until it is accepted.
//
// ------------------------------------------------------------------ mutants
//
// -DMOSAIC_CACHE_MUTANT_* selects a deliberately broken variant used to prove
// the tests detect the corresponding bug. The shipping build defines none of
// them. See results/reports/memory-subsystem-scale.md for the control table.
// ============================================================================

`default_nettype none
`resetall

module mosaic_cache #(
  parameter int  CPU_DATA_WIDTH = 64,     // bits per CPU access (a word)
  parameter int  LINE_BYTES     = 32,     // bytes per cache line, power of two
  parameter int  SETS           = 8,      // number of sets
  parameter int  WAYS           = 1,      // associativity (1 = direct-mapped)
  parameter int  ADDR_WIDTH     = 32,     // physical address width
  parameter bit  READ_ONLY      = 1'b0,   // 1 = instruction cache
  parameter int  MSHR_ENTRIES   = 4,      // outstanding-miss (MSHR) entries
  // Derived, and therefore not overridable.
  localparam int CPU_BYTES     = CPU_DATA_WIDTH / 8,
  localparam int LINE_BITS     = LINE_BYTES * 8,
  localparam int OFFSET_BITS   = $clog2(LINE_BYTES),
  localparam int INDEX_BITS    = $clog2(SETS),
  localparam int WAY_BITS      = (WAYS > 1) ? $clog2(WAYS) : 1,
  localparam int TAG_WIDTH     = ADDR_WIDTH - OFFSET_BITS - INDEX_BITS,
  localparam int WORD_BITS     = $clog2(CPU_BYTES),
  localparam int WORD_IDX_BITS = OFFSET_BITS - WORD_BITS,
  localparam int ENT_IDX_BITS  = (MSHR_ENTRIES > 1) ? $clog2(MSHR_ENTRIES) : 1,
  localparam int ENT_CNT_BITS  = $clog2(MSHR_ENTRIES + 1),
  // Waiter slots per entry, rounded up to a power of two so the small FIFO's
  // head/tail arithmetic is a mask rather than a divider. At least two, so a
  // line can always be asked for by more than one request.
  localparam int W             = 1 << $clog2(MSHR_ENTRIES + 1),
  localparam int WIDX_BITS     = (W > 1) ? $clog2(W) : 1,
  localparam int WCNT_BITS     = $clog2(W + 1)
) (
  input  logic                        clk,
  input  logic                        rst,

  // ------------------------------------------------------------ CPU request
  // `cpu_req_addr` is a byte address, but this port is word-granular: the byte
  // lane is carried by `cpu_req_wmask`, and the cache stores whole words. The
  // low log2(CPU_BYTES) address bits are therefore deliberately not decoded --
  // an aligned access has them zero, and a misaligned one is the LSU's fault,
  // not the cache's.
  input  logic                        cpu_req_valid,
  input  logic                        cpu_req_we,
  /* verilator lint_off UNUSEDSIGNAL */
  input  logic [ADDR_WIDTH-1:0]       cpu_req_addr,
  /* verilator lint_on UNUSEDSIGNAL */
  input  logic [CPU_DATA_WIDTH-1:0]   cpu_req_wdata,
  input  logic [CPU_BYTES-1:0]        cpu_req_wmask,
  output logic                        cpu_req_ready,
  output logic                        cpu_resp_valid,
  output logic [CPU_DATA_WIDTH-1:0]   cpu_resp_rdata,
  output logic                        cpu_resp_fault,

  // ----------------------------------------------------------- memory port
  output logic                        mem_req_valid,
  output logic                        mem_req_we,     // 0 = refill read, 1 = writeback
  output logic [ADDR_WIDTH-1:0]       mem_req_addr,   // line aligned
  output logic [LINE_BITS-1:0]        mem_req_wdata,
  input  logic                        mem_req_ready,
  input  logic                        mem_resp_valid,
  input  logic [LINE_BITS-1:0]        mem_resp_rdata,
  input  logic                        mem_resp_fault,

  // ------------------------------------------------------------- invalidate
  // A bypassed write to a *cacheable* address -- an atomic read-modify-write,
  // which the L1 does not carry -- changes memory without the cache seeing it.
  // A clean copy the cache still holds would then answer a later load with the
  // value from before the atomic. This port is the coherence act for that one
  // case: pulse `inv_valid` with the line address and the line is dropped
  // (written back first if it was dirty). A store through the cache never needs
  // it -- the cache itself is the writer -- so the pulse is only ever the
  // bypassed atomic. It is level-held here until it is serviced, because the
  // cache may be finishing a refill in the cycle the atomic retires.
  input  logic                        inv_valid,
  input  logic [ADDR_WIDTH-1:0]       inv_addr,

  // ----------------------------------------------------------------- flush
  // Drain every dirty line, then invalidate the whole cache. `flush_ready` is
  // high only while the cache is idle, so a flush is a valid/ready transfer.
  input  logic                        flush_valid,
  output logic                        flush_ready,
  output logic                        flush_done,
  output logic                        flush_busy,

  // -------------------------------------------------------- state inspection
  // Combinational view of one (set, way), so a testbench can assert e.g. "this
  // line is NOT valid after a failed refill" without inferring it from a later
  // access. `dbg_way` selects the way; a direct-mapped build ignores it.
  input  logic [INDEX_BITS-1:0]       dbg_index,
  input  logic [WAY_BITS-1:0]         dbg_way,
  output logic                        dbg_valid,
  output logic                        dbg_dirty,
  output logic [TAG_WIDTH-1:0]        dbg_tag,
  output logic [LINE_BITS-1:0]        dbg_data,

  // ------------------------------------------------------------ trace events
  output logic                        ev_hit,
  output logic                        ev_miss,
  output logic                        ev_coalesce,
  output logic                        ev_refill,
  output logic                        ev_writeback,
  output logic                        ev_fault,

  // ---------------------------------------------------------- observability
  output logic [ENT_CNT_BITS-1:0]     o_outstanding
);

  //  ------------------------------------------------------------------ storage
  // The arrays have NO reset and NO initial value: see "valid bits vs. data
  // RAM". `valid`/`dirty` and the victim pointers are the only per-line control
  // state and they ARE cleared by `rst`.
  logic [LINE_BITS-1:0]      data_mem [SETS][WAYS];
  logic [TAG_WIDTH-1:0]      tag_mem  [SETS][WAYS];
  logic [SETS-1:0][WAYS-1:0] valid;
  logic [SETS-1:0][WAYS-1:0] dirty;
  logic [SETS-1:0][WAY_BITS-1:0] rr;      // round-robin victim pointer per set

  // ------------------------------------------------------------------ MSHR
  // One entry per outstanding line. Entries are issued to memory in allocation
  // order and completed in that order, which is what the single-outstanding
  // port gives and what the waiters' response order relies on.
  logic                    e_valid   [MSHR_ENTRIES];
  logic [ADDR_WIDTH-1:0]   e_line    [MSHR_ENTRIES];
  logic                    e_issued  [MSHR_ENTRIES];
  logic                    e_ready   [MSHR_ENTRIES];
  logic                    e_fault   [MSHR_ENTRIES];
  logic                    e_installed [MSHR_ENTRIES];
  logic [LINE_BITS-1:0]    e_data    [MSHR_ENTRIES];
  // Per-entry waiter FIFO: the requests that asked for this line, in the order
  // they were accepted. `whead` is the oldest unanswered waiter.
  logic [WIDX_BITS-1:0]       whead  [MSHR_ENTRIES];
  logic [WCNT_BITS-1:0]       wcnt   [MSHR_ENTRIES];
  logic [WORD_IDX_BITS-1:0]   wword  [MSHR_ENTRIES][W];
  logic                       wwe    [MSHR_ENTRIES][W];
  logic [CPU_BYTES-1:0]       wmask  [MSHR_ENTRIES][W];
  logic [CPU_DATA_WIDTH-1:0]  wdata  [MSHR_ENTRIES][W];

  // One-deep hit response register (see "response channel" in the header).
  logic                      hit_resp_valid_r;
  logic                      hit_resp_fault_r;
  logic [CPU_DATA_WIDTH-1:0] hit_resp_data_r;

  // Elaboration guards: a geometry this module cannot represent is an
  // elaboration error, not a silent misbuild.
  generate
    if ((1 << OFFSET_BITS) != LINE_BYTES) begin : g_bad_line_bytes
      mosaic_cache_contract_violation u_line_bytes ();
    end
    if ((1 << INDEX_BITS) != SETS) begin : g_bad_sets
      mosaic_cache_contract_violation u_sets ();
    end
    if ((TAG_WIDTH < 1) || ((1 << WORD_BITS) != CPU_BYTES)) begin : g_bad_tag
      mosaic_cache_contract_violation u_tag ();
    end
    if ((LINE_BYTES % CPU_BYTES) != 0) begin : g_bad_words
      mosaic_cache_contract_violation u_words ();
    end
    if ((WAYS < 1) || ((WAYS > 1) && ((1 << WAY_BITS) != WAYS))) begin : g_bad_ways
      mosaic_cache_contract_violation u_ways ();
    end
    if ((MSHR_ENTRIES < 1) || (W < 2)) begin : g_bad_entries
      mosaic_cache_contract_violation u_entries ();
    end
  endgenerate

  // ---------------------------------------------------------------- FSM state
  typedef enum logic [2:0] {
    ST_RUN        = 3'd0,   // normal: accepting requests, issuing refills
    ST_WB         = 3'd1,   // evicting a dirty victim before an install
    ST_INSTALL    = 3'd2,   // writing the fetched line into the array
    ST_FLUSH_SCAN = 3'd3,   // walking every (set, way) during a flush
    ST_FLUSH_WB   = 3'd4,   // writing back one dirty line during a flush
    ST_FLUSH_DONE = 3'd5,   // one-cycle flush completion pulse
    ST_INV_WB     = 3'd6,   // writing back a dirty line before invalidating it
    ST_INV_DROP   = 3'd7    // dropping the invalidated line
  } state_e;

  state_e state;

  // The install in progress (registered at the ST_RUN -> ST_WB/ST_INSTALL edge).
  logic [ENT_IDX_BITS-1:0] inst_idx;
  logic [WAY_BITS-1:0]     inst_way;
  // The read in flight.
  logic                    rd_pending;
  logic [ENT_IDX_BITS-1:0] rd_idx;
  // The flush walk.
  logic [INDEX_BITS:0]     flush_set;
  logic [WAY_BITS:0]       flush_way;
  // The pending invalidate (a bypassed atomic's line).
  logic                    inv_pending_q;
  /* verilator lint_off UNUSEDSIGNAL */
  logic [ADDR_WIDTH-1:0]   inv_addr_q;   // only the set/tag bits are decoded
  /* verilator lint_on UNUSEDSIGNAL */
  logic [INDEX_BITS-1:0]   inv_idx;
  logic [WAY_BITS-1:0]     inv_way;

  // --------------------------------------------------------- request decode
  logic [INDEX_BITS-1:0]    req_index;
  logic [TAG_WIDTH-1:0]     req_tag;
  logic [WORD_IDX_BITS-1:0] req_word;
  logic [ADDR_WIDTH-1:0]    req_line;
  assign req_index = cpu_req_addr[OFFSET_BITS + INDEX_BITS - 1 : OFFSET_BITS];
  assign req_tag   = cpu_req_addr[ADDR_WIDTH - 1 : OFFSET_BITS + INDEX_BITS];
  assign req_word  = cpu_req_addr[OFFSET_BITS - 1 : WORD_BITS];
  assign req_line  = {cpu_req_addr[ADDR_WIDTH - 1 : OFFSET_BITS], {OFFSET_BITS{1'b0}}};

  // Set/tag of a line-aligned address are plain bit selects; the set of the
  // install candidate is `oldest_set` below.

  // Way that holds the requested line, and whether any does.
  logic [WAYS-1:0]     way_hit;
  logic                hit_c;
  logic [WAY_BITS-1:0] hit_way;
  always_comb begin
    way_hit = '0;
    for (int unsigned w = 0; w < WAYS; w++) begin
      if (valid[req_index][w] && (tag_mem[req_index][w] == req_tag)) way_hit[w] = 1'b1;
    end
    hit_c   = |way_hit;
    hit_way = '0;
    for (int unsigned w = 0; w < WAYS; w++) begin
      if (!hit_way[0] && way_hit[w]) hit_way = w[WAY_BITS-1:0];
    end
  end

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
    // Ascending, so the lowest free entry is taken: allocation order is index
    // order and therefore the response order a single-issue consumer expects.
    for (int i = 0; i < MSHR_ENTRIES; i++) begin
      // A faulted entry is not coalesced onto: its line is not being fetched
      // any more, so a new request must allocate afresh and retry the refill.
`ifdef MOSAIC_CACHE_MUTANT_COALESCE_LOW_BITS
      // MUTANT (V-062 control): two requests coalesce when their low address
      // bits (set + offset) match, so two *different* lines that happen to
      // share a set and an offset join one entry and the second consumer is
      // answered from the first line -- "matching on low address bits alone".
      if (!dup_found && e_valid[i] && !e_installed[i] && !e_fault[i] &&
          (e_line[i][OFFSET_BITS+INDEX_BITS-1:0] ==
           req_line[OFFSET_BITS+INDEX_BITS-1:0])) begin
`else
      if (!dup_found && e_valid[i] && !e_installed[i] && !e_fault[i] &&
          (e_line[i] == req_line)) begin
`endif
        dup_found = 1'b1;
        dup_idx   = i[ENT_IDX_BITS-1:0];
      end
      if (!free_found && !e_valid[i]) begin
        free_found = 1'b1;
        free_idx   = i[ENT_IDX_BITS-1:0];
      end
    end
  end

  // Room for one more waiter on a found entry.
  logic dup_room;
  assign dup_room = dup_found && (wcnt[dup_idx] != WCNT_BITS'(W));

  // The FIFO slot a new waiter goes into. W is a power of two, so the
  // WIDX_BITS-wide sum wraps modulo W on its own.
  logic [WIDX_BITS-1:0] ap_slot;
  assign ap_slot = whead[dup_idx] + wcnt[dup_idx][WIDX_BITS-1:0];

  logic is_icache_store;
  assign is_icache_store = READ_ONLY && cpu_req_we;

  // A miss (no hit) is accepted when it can coalesce onto a live entry or gets
  // a free entry. A hit is always accepted. A store to a read-only cache is
  // accepted and answered with a fault, as I-042 specified.
  logic miss_accept_c;
  assign miss_accept_c = !hit_c && (dup_room || free_found);
// Either define selects the stalled-hit arm: the mutant for the control, the
// blocking define for the before/after measurement.
`ifdef MOSAIC_CACHE_BLOCKING
`define MOSAIC_CACHE_STALL_HITS
`endif
`ifdef MOSAIC_CACHE_MUTANT_HIT_BLOCKS_MISS
`define MOSAIC_CACHE_STALL_HITS
`endif
`ifdef MOSAIC_CACHE_STALL_HITS
  // NEGATIVE CONTROL / MEASUREMENT ARM: a hit is stalled behind an unrelated
  // miss -- the exact regression the non-blocking read path exists to remove.
  // `cpu_req_ready` drops for the whole window in which a miss is outstanding,
  // so a demand that hits an existing line waits for a line it never asked for.
  // `MOSAIC_CACHE_BLOCKING` selects the same behaviour for the before/after
  // measurement in results/reports/memory-subsystem-scale.md; the mutant define
  // selects it for the control.
  logic miss_inflight;
  always_comb begin
    miss_inflight = 1'b0;
    for (int unsigned i = 0; i < MSHR_ENTRIES; i++) begin
      if (e_valid[i] && !e_ready[i] && !e_fault[i]) miss_inflight = 1'b1;
    end
  end
  assign cpu_req_ready = (state == ST_RUN) && !flush_valid && !miss_inflight &&
                         (is_icache_store || hit_c || miss_accept_c);
`else
  assign cpu_req_ready = (state == ST_RUN) && !flush_valid &&
                         (is_icache_store || hit_c || miss_accept_c);
`endif
`undef MOSAIC_CACHE_STALL_HITS

  logic cpu_accept_c;
  assign cpu_accept_c = cpu_req_valid && cpu_req_ready;

  // --------------------------------------------------------- memory requests
  // A refill read for the oldest entry that has waiters and has not been issued.
  logic                    rd_issue_valid;
  logic [ADDR_WIDTH-1:0]   rd_issue_addr;
  logic [ENT_IDX_BITS-1:0] rd_issue_idx;
  always_comb begin
    rd_issue_valid = 1'b0;
    rd_issue_addr  = '0;
    rd_issue_idx   = '0;
    for (int unsigned i = 0; i < MSHR_ENTRIES; i++) begin
      if (!rd_issue_valid && e_valid[i] && !e_issued[i] && !e_ready[i] && !e_fault[i] &&
          (wcnt[i] != {WCNT_BITS{1'b0}})) begin
        rd_issue_valid = 1'b1;
        rd_issue_addr  = e_line[i];
        rd_issue_idx   = i[ENT_IDX_BITS-1:0];
      end
    end
  end

`ifdef MOSAIC_CACHE_MUTANT_WRONG_REFILL
  // MUTANT: the refill reads the line *after* the one that missed, so every
  // line is filled with its neighbour's bytes.
  logic [ADDR_WIDTH-1:0] rd_addr_eff;
  assign rd_addr_eff = rd_issue_addr ^ ADDR_WIDTH'(LINE_BYTES);
`else
  logic [ADDR_WIDTH-1:0] rd_addr_eff;
  assign rd_addr_eff = rd_issue_addr;
`endif

  // ------------------------------------------------- invalidate lookup
  logic [INDEX_BITS-1:0] inv_index;
  logic [TAG_WIDTH-1:0]  inv_tag;
  logic [WAYS-1:0]       inv_way_hit;
  logic                  inv_hit;
  assign inv_index = inv_addr_q[OFFSET_BITS + INDEX_BITS - 1 : OFFSET_BITS];
  assign inv_tag   = inv_addr_q[ADDR_WIDTH - 1 : OFFSET_BITS + INDEX_BITS];
  always_comb begin
    inv_way_hit = '0;
    for (int unsigned w = 0; w < WAYS; w++) begin
      if (valid[inv_index][w] && (tag_mem[inv_index][w] == inv_tag)) inv_way_hit[w] = 1'b1;
    end
    inv_hit = |inv_way_hit;
    inv_way = '0;
    for (int unsigned w = 0; w < WAYS; w++) begin
      if (!inv_way[0] && inv_way_hit[w]) inv_way = w[WAY_BITS-1:0];
    end
  end
  assign inv_idx = inv_index;

  // ------------------------------------------------------ install selection
  logic                    have_oldest;
  logic [ENT_IDX_BITS-1:0] oldest_idx;
  always_comb begin
    have_oldest = 1'b0;
    oldest_idx  = '0;
    for (int i = MSHR_ENTRIES - 1; i >= 0; i--) begin
      if (e_valid[i]) begin
        have_oldest = 1'b1;
        oldest_idx  = i[ENT_IDX_BITS-1:0];
      end
    end
  end

  logic inst_avail;
  assign inst_avail = (state == ST_RUN) && have_oldest && e_ready[oldest_idx] &&
                      !e_installed[oldest_idx] && !rd_pending;

  logic [INDEX_BITS-1:0] oldest_set;
  assign oldest_set = e_line[oldest_idx][OFFSET_BITS + INDEX_BITS - 1 : OFFSET_BITS];

  logic [INDEX_BITS-1:0] inst_set;
  assign inst_set = e_line[inst_idx][OFFSET_BITS + INDEX_BITS - 1 : OFFSET_BITS];

  // ------------------------------------------------------------------------
  // The victim of the install in progress, and its address/data.
  logic [ADDR_WIDTH-1:0] inst_victim_addr;
  logic [LINE_BITS-1:0]  inst_victim_data;
  logic [ADDR_WIDTH-1:0] flush_addr;
  assign flush_addr = {tag_mem[flush_set[INDEX_BITS-1:0]][flush_way[WAY_BITS-1:0]],
                       flush_set[INDEX_BITS-1:0], {OFFSET_BITS{1'b0}}};
  assign inst_victim_addr = (state == ST_FLUSH_WB)
                              ? flush_addr
                            : (state == ST_INV_WB)
                              ? {tag_mem[inv_idx][inv_way], inv_idx, {OFFSET_BITS{1'b0}}}
                              : {tag_mem[inst_set][inst_way], inst_set, {OFFSET_BITS{1'b0}}};
  assign inst_victim_data = (state == ST_FLUSH_WB)
                              ? data_mem[flush_set[INDEX_BITS-1:0]][flush_way[WAY_BITS-1:0]]
                            : (state == ST_INV_WB)
                              ? data_mem[inv_idx][inv_way]
                              : data_mem[inst_set][inst_way];

  // The memory port: a refill read, or a writeback.
  logic wb_active;
  assign wb_active = (state == ST_WB) || (state == ST_FLUSH_WB) || (state == ST_INV_WB);

  assign mem_req_valid = wb_active ||
                         (rd_issue_valid && !rd_pending && !inst_avail && (state == ST_RUN));
  assign mem_req_we    = wb_active;
  assign mem_req_addr  = wb_active ? inst_victim_addr : rd_addr_eff;
`ifdef MOSAIC_CACHE_MUTANT_DIRTY_DROP
  // MUTANT: the eviction is issued, but with zeroed data, so the dirty bytes
  // never reach memory.
  assign mem_req_wdata = {LINE_BITS{1'b0}};
`else
  assign mem_req_wdata = inst_victim_data;
`endif

  // The line the install in progress will write: the refill bytes with every
  // store waiter's bytes merged in. A slot is live when it lies in the FIFO
  // window [head, head+cnt); the WIDX_BITS-wide difference wraps modulo W.
  logic [LINE_BITS-1:0] inst_line_data;
  logic                 inst_dirty;
  always_comb begin
    inst_line_data = e_data[inst_idx];
    inst_dirty     = 1'b0;
    for (int unsigned k = 0; k < W; k++) begin
      if ((k[WIDX_BITS-1:0] - whead[inst_idx]) < wcnt[inst_idx][WIDX_BITS-1:0]) begin
        if (wwe[inst_idx][k] && !READ_ONLY) begin
          for (int unsigned b = 0; b < CPU_BYTES; b++) begin
`ifdef MOSAIC_CACHE_MUTANT_STORE_MASK_DROP
            // MUTANT (V-062 control): every byte lane of a store waiter is
            // written whatever `wmask` says, so a partial store clobbers the
            // bytes it did not select -- "losing byte enables".
            inst_line_data[(int'(wword[inst_idx][k]) * CPU_BYTES + b) * 8 +: 8]
              = wdata[inst_idx][k][8*b +: 8];
`else
            if (wmask[inst_idx][k][b]) begin
              inst_line_data[(int'(wword[inst_idx][k]) * CPU_BYTES + b) * 8 +: 8]
                = wdata[inst_idx][k][8*b +: 8];
            end
`endif
          end
          inst_dirty = 1'b1;
        end
      end
    end
  end

  // -------------------------------------------------------------- responses
  // One response per cycle: the hit register wins, a drained waiter waits.
  logic                      drain_valid;
  logic [CPU_DATA_WIDTH-1:0] drain_rdata;
  logic                      drain_fault;
  logic [WIDX_BITS-1:0]      drain_slot;
  always_comb begin
    drain_valid = 1'b0;
    drain_rdata = '0;
    drain_fault = 1'b0;
    drain_slot  = '0;
    if (have_oldest && (wcnt[oldest_idx] != {WCNT_BITS{1'b0}}) &&
        (e_fault[oldest_idx] || e_installed[oldest_idx]) && !hit_resp_valid_r) begin
      drain_valid = 1'b1;
      drain_fault = e_fault[oldest_idx];
`ifdef MOSAIC_CACHE_MUTANT_WRONG_WAITER
      // MUTANT: every waiter on a coalesced line is answered from the FIFO's
      // first slot, so a request that joined an in-flight miss gets another
      // requester's word -- "a coalesced response delivered to the wrong
      // requester".
      drain_slot  = '0;
`else
      drain_slot  = whead[oldest_idx];
`endif
      drain_rdata = drain_fault
                      ? {CPU_DATA_WIDTH{1'b0}}
                      : e_data[oldest_idx][int'(wword[oldest_idx][drain_slot]) *
                                           CPU_DATA_WIDTH +: CPU_DATA_WIDTH];
    end
  end

  assign cpu_resp_valid = hit_resp_valid_r || drain_valid;
  assign cpu_resp_fault = hit_resp_valid_r ? hit_resp_fault_r : drain_fault;
  assign cpu_resp_rdata = hit_resp_valid_r ? hit_resp_data_r : drain_rdata;

  // The oldest entry, once installed or faulted, frees as soon as its last
  // waiter has been answered. `drain_free_c` is that last cycle.
  logic drain_free_c;
  assign drain_free_c = drain_valid && (wcnt[oldest_idx] == WCNT_BITS'(1));

  // ------------------------------------------------------------------ flush
  assign flush_ready = (state == ST_RUN) && !have_oldest && !rd_pending &&
                       !hit_resp_valid_r && !inv_pending_q;
  assign flush_busy  = (state == ST_FLUSH_SCAN) || (state == ST_FLUSH_WB);

  // ------------------------------------------------------------- debug view
  assign dbg_valid = valid[dbg_index][dbg_way];
  assign dbg_dirty = dirty[dbg_index][dbg_way];
  assign dbg_tag   = tag_mem[dbg_index][dbg_way];
  assign dbg_data  = data_mem[dbg_index][dbg_way];

  // ------------------------------------------------------------ outstanding
  logic [ENT_CNT_BITS:0] outstanding_sum;
  always_comb begin
    outstanding_sum = '0;
    for (int unsigned i = 0; i < MSHR_ENTRIES; i++) begin
      if (e_valid[i]) outstanding_sum = outstanding_sum + {{ENT_CNT_BITS{1'b0}}, 1'b1};
    end
  end
  assign o_outstanding = outstanding_sum[ENT_CNT_BITS-1:0];

  // ---------------------------------------------------------------- FSM
  always_ff @(posedge clk) begin
    ev_hit       <= 1'b0;
    ev_miss      <= 1'b0;
    ev_coalesce  <= 1'b0;
    ev_refill    <= 1'b0;
    ev_writeback <= 1'b0;
    ev_fault     <= 1'b0;
    flush_done   <= 1'b0;

    if (rst) begin
      state            <= ST_RUN;
      valid            <= '0;
      dirty            <= '0;
      rr               <= '0;
      rd_pending       <= 1'b0;
      rd_idx           <= '0;
      inst_idx         <= '0;
      inst_way         <= '0;
      flush_set        <= '0;
      flush_way        <= '0;
      inv_pending_q    <= 1'b0;
      inv_addr_q       <= '0;
      hit_resp_valid_r <= 1'b0;
      hit_resp_fault_r <= 1'b0;
      hit_resp_data_r  <= '0;
      for (int unsigned i = 0; i < MSHR_ENTRIES; i++) begin
        e_valid[i]     <= 1'b0;
        e_line[i]      <= '0;
        e_issued[i]    <= 1'b0;
        e_ready[i]     <= 1'b0;
        e_fault[i]     <= 1'b0;
        e_installed[i] <= 1'b0;
        e_data[i]      <= {LINE_BITS{1'b0}};
        whead[i]       <= '0;
        wcnt[i]        <= '0;
        for (int unsigned k = 0; k < W; k++) begin
          wword[i][k] <= '0;
          wwe[i][k]   <= 1'b0;
          wmask[i][k] <= '0;
          wdata[i][k] <= '0;
        end
      end
      // data_mem and tag_mem are deliberately NOT reset (see header).
    end else begin
      // ---------------------------------------------------- hit response
      if (cpu_accept_c && is_icache_store) begin
        hit_resp_valid_r <= 1'b1;
        hit_resp_fault_r <= 1'b1;
        hit_resp_data_r  <= '0;
      end else if (cpu_accept_c && hit_c) begin
        hit_resp_valid_r <= 1'b1;
        hit_resp_fault_r <= 1'b0;
        hit_resp_data_r  <= data_mem[req_index][hit_way][int'(req_word) * CPU_DATA_WIDTH +: CPU_DATA_WIDTH];
      end else if (hit_resp_valid_r) begin
        hit_resp_valid_r <= 1'b0;
      end

      // ---------------------------------------------------- store-hit merge
      if (cpu_accept_c && hit_c && !READ_ONLY && cpu_req_we) begin
        for (int unsigned b = 0; b < CPU_BYTES; b++) begin
          if (cpu_req_wmask[b]) begin
            data_mem[req_index][hit_way][(int'(req_word) * CPU_BYTES + b) * 8 +: 8]
              <= cpu_req_wdata[8*b +: 8];
          end
        end
        dirty[req_index][hit_way] <= 1'b1;
      end

      // ---------------------------------------------------- miss allocation
      if (cpu_accept_c && !hit_c && !is_icache_store) begin
        if (dup_room) begin
          wword[dup_idx][ap_slot]   <= req_word;
          wwe[dup_idx][ap_slot]     <= cpu_req_we;
          wmask[dup_idx][ap_slot]   <= cpu_req_wmask;
          wdata[dup_idx][ap_slot]   <= cpu_req_wdata;
          wcnt[dup_idx]             <= wcnt[dup_idx] + {{(WCNT_BITS-1){1'b0}}, 1'b1};
        end else if (free_found) begin
          e_valid[free_idx]     <= 1'b1;
          e_line[free_idx]      <= req_line;
          e_issued[free_idx]    <= 1'b0;
          e_ready[free_idx]     <= 1'b0;
          e_fault[free_idx]     <= 1'b0;
          e_installed[free_idx] <= 1'b0;
          e_data[free_idx]      <= {LINE_BITS{1'b0}};
          whead[free_idx]       <= '0;
          wcnt[free_idx]        <= {{(WCNT_BITS-1){1'b0}}, 1'b1};
          wword[free_idx][0]    <= req_word;
          wwe[free_idx][0]      <= cpu_req_we;
          wmask[free_idx][0]    <= cpu_req_wmask;
          wdata[free_idx][0]    <= cpu_req_wdata;
        end
      end

      // ------------------------------------------------ waiter drain / free
      if (drain_valid) begin
        whead[oldest_idx] <= whead[oldest_idx] + 1'b1;
        wcnt[oldest_idx]  <= wcnt[oldest_idx] - {{(WCNT_BITS-1){1'b0}}, 1'b1};
        if (drain_free_c) e_valid[oldest_idx] <= 1'b0;
      end

      // ------------------------------------------------------- invalidate
      if (inv_valid && !inv_pending_q) begin
        inv_pending_q <= 1'b1;
        inv_addr_q    <= inv_addr;
      end

      // ------------------------------------------------------------- memory
      if (mem_req_valid && mem_req_ready && !wb_active) begin
        rd_pending             <= 1'b1;
        rd_idx                 <= rd_issue_idx;
        e_issued[rd_issue_idx] <= 1'b1;
      end
      if (mem_resp_valid && rd_pending) begin
        rd_pending <= 1'b0;
        if (mem_resp_fault) begin
`ifdef MOSAIC_CACHE_MUTANT_FAULT_VALID
          // MUTANT: a faulted refill is still installed as if it had succeeded.
          e_ready[rd_idx] <= 1'b1;
          e_fault[rd_idx] <= 1'b0;
          e_data[rd_idx]  <= mem_resp_rdata;
`else
          e_fault[rd_idx] <= 1'b1;
          ev_fault        <= 1'b1;
`endif
        end else begin
          e_ready[rd_idx] <= 1'b1;
          e_data[rd_idx]  <= mem_resp_rdata;
        end
      end

      // ------------------------------------------------------- trace pulses
      if (cpu_accept_c && hit_c && !is_icache_store) ev_hit <= 1'b1;
      if (cpu_accept_c && !hit_c && !is_icache_store) begin
        ev_miss <= 1'b1;
        if (dup_room) ev_coalesce <= 1'b1;
      end

      // -------------------------------------------------------------- FSM
      case (state)
        ST_RUN: begin
          if (inv_pending_q) begin
            if (inv_hit) begin
              // A dirty copy must reach memory before its tag is dropped, or
              // the atomic's own write would be the only survivor and the
              // cache's unflushed bytes would be lost.
              if (dirty[inv_idx][inv_way] && !READ_ONLY) begin
                state <= ST_INV_WB;
              end else begin
                state <= ST_INV_DROP;
              end
            end else begin
              inv_pending_q <= 1'b0;   // nothing cached for this line
            end
          end else if (inst_avail) begin
            inst_idx <= oldest_idx;
            inst_way <= rr[oldest_set];
            // A dirty victim must reach memory before the install reuses its way.
            if (valid[oldest_set][rr[oldest_set]] && dirty[oldest_set][rr[oldest_set]] &&
                !READ_ONLY) begin
              state <= ST_WB;
            end else begin
              state <= ST_INSTALL;
            end
          end else if (flush_valid && !have_oldest && !rd_pending && !hit_resp_valid_r) begin
            flush_set <= '0;
            flush_way <= '0;
            state     <= ST_FLUSH_SCAN;
          end
        end

        ST_WB: begin
          if (mem_req_ready) begin
            ev_writeback      <= 1'b1;
            dirty[inst_set][inst_way] <= 1'b0;
            state             <= ST_INSTALL;
          end
        end

        ST_INSTALL: begin
          valid[inst_set][inst_way] <= 1'b1;
          tag_mem[inst_set][inst_way] <= e_line[inst_idx][ADDR_WIDTH-1:OFFSET_BITS+INDEX_BITS];
          data_mem[inst_set][inst_way] <= inst_line_data;
          dirty[inst_set][inst_way] <= inst_dirty && !READ_ONLY;
          e_installed[inst_idx] <= 1'b1;
          e_data[inst_idx]      <= inst_line_data;
          rr[inst_set]          <= (inst_way == WAY_BITS'(WAYS-1)) ? '0 : (inst_way + 1'b1);
          ev_refill             <= 1'b1;
          state                 <= ST_RUN;
        end

        ST_FLUSH_SCAN: begin
          if (flush_set == (INDEX_BITS + 1)'(SETS)) begin
            state <= ST_FLUSH_DONE;
          end else if (valid[flush_set[INDEX_BITS-1:0]][flush_way[WAY_BITS-1:0]] &&
                       dirty[flush_set[INDEX_BITS-1:0]][flush_way[WAY_BITS-1:0]]) begin
            state <= ST_FLUSH_WB;
          end else begin
            valid[flush_set[INDEX_BITS-1:0]][flush_way[WAY_BITS-1:0]] <= 1'b0;
            dirty[flush_set[INDEX_BITS-1:0]][flush_way[WAY_BITS-1:0]] <= 1'b0;
            if (flush_way == (WAY_BITS+1)'(WAYS-1)) begin
              flush_way <= '0;
              flush_set <= flush_set + 1'b1;
            end else begin
              flush_way <= flush_way + 1'b1;
            end
          end
        end

        ST_FLUSH_WB: begin
          if (mem_req_ready) begin
            ev_writeback <= 1'b1;
            valid[flush_set[INDEX_BITS-1:0]][flush_way[WAY_BITS-1:0]] <= 1'b0;
            dirty[flush_set[INDEX_BITS-1:0]][flush_way[WAY_BITS-1:0]] <= 1'b0;
            if (flush_way == (WAY_BITS+1)'(WAYS-1)) begin
              flush_way <= '0;
              flush_set <= flush_set + 1'b1;
            end else begin
              flush_way <= flush_way + 1'b1;
            end
            state <= ST_FLUSH_SCAN;
          end
        end

        ST_INV_WB: begin
          if (mem_req_ready) begin
            ev_writeback <= 1'b1;
            dirty[inv_idx][inv_way] <= 1'b0;
            state <= ST_INV_DROP;
          end
        end

        ST_INV_DROP: begin
          valid[inv_idx][inv_way] <= 1'b0;
          dirty[inv_idx][inv_way] <= 1'b0;
          inv_pending_q <= 1'b0;
          state <= ST_RUN;
        end

        ST_FLUSH_DONE: begin
          flush_done <= 1'b1;
          state      <= ST_RUN;
        end

        default: state <= ST_RUN;
      endcase
    end
  end

endmodule

`resetall
