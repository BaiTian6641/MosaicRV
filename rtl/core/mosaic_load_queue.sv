// ============================================================================
// mosaic_load_queue -- work package I-035: the load queue and store-to-load
// forwarding.
//
// A load is the only instruction whose result depends on what every *earlier*
// instruction did, because a store that has executed but not yet left the store
// queue holds the architecturally newest value of its bytes. This module is the
// place that decides, byte by byte, whether a load's byte comes from the most
// recent older store that covers it or from memory -- and it must never do so
// from a store the load has not yet reached in program order.
//
// ------------------------------------------------------------ the three rules
//
// The card names three inputs, and each is implemented as one expression here:
//
//   1. **Byte overlap.** Coverage is a byte range test, not a word match. A
//      store covers the load's byte at `x` exactly when
//      `store_addr <= x < store_addr + size_bytes(store_size)`. Two accesses
//      alias when their ranges intersect, whatever their sizes -- a 1-byte
//      store under an 8-byte load, a 4-byte store over a 2-byte load, an
//      unaligned start on either side.
//
//   2. **The youngest older store.** Among the resident store-queue entries
//      that are *older* than the load and cover the byte, the one at the
//      highest store-queue position wins. "Older" is decided from the frozen
//      macro identity (`mosaic_id_pkg::macro_id_t`): `rob_gen` advances once per
//      ROB allocation (mosaic_rob, `gen_counter`), so two identities are ordered
//      by their generation distance modulo `2**ROB_GEN_W`, and two uops of the
//      *same* macro by `uop_index`. The ROB's own contract (rob_generation
//      modulus 128 against at most 64 live entries) is what makes the
//      half-modulus comparison exact; this module relies on it rather than
//      restating it.
//
//      A store *younger* than the load is never a source. That is the card's
//      second Fail mode, and it is structural here: the `FwdOlder` predicate is
//      a conjunct of the byte's source selection, so a younger covering store
//      cannot be selected even when it is the youngest resident match.
//
//   3. **A store whose address is not known.** Such a store might be older and
//      might overlap, so no byte may be taken from memory while one is
//      resident. The store queue exports exactly this fact as `fwd_blocked_o`
//      ("some resident entry has no address yet"), and the load queue consumes
//      it as a whole-load block: while it is high the load is not completed,
//      it is *replayed* (re-issued to the memory endpoint), and the replay is
//      counted. The card's conservative baseline is "a load waits for all
//      relevant older store addresses to be known"; the store queue's flag is
//      coarser (it covers younger entries too), and taking the coarser flag is
//      the safe direction.
//
//      A second, narrower block is needed for correctness and is *not* the
//      card's unknown-address rule: a store whose address *is* known, whose
//      range covers a load byte, and whose *data* is not captured yet. It is
//      not a forwarding source (nothing to forward) and memory is not a source
//      either (the store will write different bytes), so the load must wait for
//      it exactly as it waits for an unknown address. `architecture-review.md`
//      §7 names "address ready but data not ready" as a case forwarding must
//      cover, and this is where it is covered.
//
// --------------------------------------------------- the store-side interface
//
// The card requires this module to consume the store queue's forwarding query
// (`fwd_query_addr_i`/`fwd_query_size_i` -> `fwd_valid_o`/`fwd_data_o`/
// `fwd_blocked_o`), and it is consumed: the load queue drives the query with the
// address and size of the load at the head and takes `fwd_blocked_o` as the
// unknown-address policy above. Every cycle of the case also compares the
// query's `fwd_valid_o`/`fwd_data_o` against the rule applied to the store
// queue's own entries, so the port is exercised, not merely declared.
//
// The byte-level *selection*, however, cannot be made through that port, and
// saying so is part of the contract rather than an omission:
//
//   * `fwd_valid_o` answers for a byte range that one store **fully contains**.
//     A load that spans two stores -- the card's "partial overlap, both
//     directions" coverage -- is never fully contained by any one store, so the
//     port answers "no" and a consumer that trusted it would take every byte
//     from memory. Partial coverage is answered only by asking about
//     sub-ranges, and the port takes one query per cycle.
//   * The port's rule is "the youngest **resident** entry". The store queue
//     cannot know where the load sits in program order -- its header says so
//     explicitly -- so its answer includes stores *younger* than the load, which
//     this module must not forward from. Filtering them out requires the
//     entries' identities, which the query does not return.
//
// So the byte-level search is made over the store queue's exported entry view
// (`o_entry_pay` -> `sq_entry_pay_i`, `o_count` -> `sq_count_i`), which carries
// each entry's address, size, data, both readiness bits and its identity -- the
// five things the youngest-older-per-byte rule needs. This is the store queue's
// *observation* port, and its header says a functional consumer has no business
// reading it; that statement is about the store path ("the drain port is the
// interface"). For the load path there is no other port that carries program
// order, and inventing a second copy of the store queue inside this one would be
// worse. The tension is recorded in results/reports/I-035-lq.md rather than
// hidden; the query port remains the source of the blocking policy, and the two
// views are cross-checked.
//
// ------------------------------------------- what is deliberately NOT here
//
//   * **Speculative disambiguation and violation replay** are I-036
//     (`lsu.late_alias_replay`). A load here never passes an older store whose
//     address is unknown; it waits. There is therefore no "the load was
//     executed too early" state, no dependence-violation detector and no replay
//     of younger work. The replay this module implements is the narrower one the
//     card names: re-issuing a load whose bytes were not all available once the
//     block lifts.
//   * **FENCE / FENCE.I ordering** is I-037. This module knows nothing about
//     fences; a fence that must order a load behind a store is the fence
//     module's business.
//   * **Multiple outstanding transactions.** The memory endpoint (I-033) holds
//     one transaction at a time and this queue issues one load at a time, so
//     completion is in order and there is no reorder buffer here. I-043's
//     nonblocking path changes that, and the exactly-once argument for issuing
//     would have to be re-made for it.
//   * **Faults are reported, not taken.** A fault in the endpoint's response is
//     carried to the result port with its cause and `tval`; the architectural
//     trap decision belongs to the ROB/retire path.
//
// ------------------------------------------------------------- the lifecycle
//
//     allocate --(head, not in flight)--> issue to endpoint
//              --(response, block clear)--> merge + extend --> result
//              --(response, blocked)------> replay (re-issue)
//
// `alloc_ready_o` is a function of the registered occupancy alone, so there is
// no combinational path from the memory side back into the allocator -- the
// trade mosaic_result_fifo and mosaic_store_queue both document.
//
// The merged value is built in the *load's* byte order (byte `i` at bits
// `8i..8i+7`): a byte that an older covering store owns comes from that store's
// payload at its offset inside the store, and every other byte comes from the
// endpoint's response, whose low bytes are the memory bytes at
// `load_addr .. load_addr + n - 1` in that same order (mosaic_lsu_endpoint
// extracts by shifting the lane-aligned memory doubleword down to lane 0). The
// merged word is then sign- or zero-extended by size, exactly as the endpoint
// extends a pure memory value. `result_fwd_mask_o` reports, per byte, which of
// the two it came from -- the card asks for that indication and the case checks
// it byte by byte.
//
// ------------------------------------------------------------- negative controls
//
// -DMOSAIC_LQ_MUTANT_<n> injects one defect the case must detect. None is
// defined in the shipping build. The table with real build commands, binary
// hashes and exit codes is in results/reports/I-035-lq.md:
//
//   YOUNGER_FORWARDS    the oldest-youngest filter is dropped: the youngest
//                       resident match wins even when it is younger than the
//                       load.
//   ALIGNED_WORD_ONLY   coverage compares the aligned doubleword instead of the
//                       byte range, so a partial overlap inside one doubleword
//                       is treated as full coverage.
//   IGNORE_BLOCK        the store queue's `fwd_blocked_o` is dropped, so a load
//                       is completed from memory while an older store's address
//                       is still unknown.
//   STALE_FORWARD       the forwarded bytes are latched when the load is issued
//                       and reused at completion, so a store whose address
//                       resolves in between is not picked up.
//   MEMORY_OVER_STORE   the store source is ignored: every byte is taken from
//                       the memory response even when a legal older store
//                       covers it.
// ============================================================================

`ifndef MOSAIC_LOAD_QUEUE_SV_
`define MOSAIC_LOAD_QUEUE_SV_

`default_nettype none
`resetall

/* verilator lint_off UNUSEDPARAM */
// The generated header declares one localparam per knob for the whole project.
// This module names the LSU subset it needs; the rest belong to other modules
// and are unused *here* by construction, not by omission.
`include "mosaic_cfg_pkg.svh"
/* verilator lint_on UNUSEDPARAM */

module mosaic_load_queue #(
    // Geometry, from the generated packages only. See mosaic_store_queue for why
    // these are `localparam` and why the identity widths come from mosaic_uop_pkg
    // rather than from a second include of the identity header.
    localparam int unsigned ENTRIES     = mosaic_cfg_pkg::MOSAIC_LQ_ENTRIES,
    localparam int unsigned SQ_ENTRIES  = mosaic_cfg_pkg::MOSAIC_SQ_ENTRIES,
    localparam int unsigned XLEN        = mosaic_cfg_pkg::MOSAIC_XLEN,
    localparam int unsigned ROB_GEN_W   = mosaic_uop_pkg::ROB_GEN_W,
    localparam int unsigned UOP_INDEX_W = mosaic_uop_pkg::UOP_INDEX_W,
    localparam int unsigned ID_W        = $bits(mosaic_uop_pkg::uop_id_t),

    // Derived, so not overridable. CNT_W can hold 0..ENTRIES.
    localparam int unsigned CNT_W       = $clog2(ENTRIES + 1),
    localparam int unsigned IDX_W       = (ENTRIES <= 1) ? 1 : $clog2(ENTRIES),
    localparam int unsigned SQ_CNT_W    = $clog2(SQ_ENTRIES + 1),
    localparam int unsigned BYTES       = XLEN / 8,

    // The store queue's exported entry layout (`o_entry_pay`), least significant
    // field bit first, exactly as mosaic_store_queue's header documents it. It is
    // re-expressed here rather than included because it is a wire layout, not a
    // package; the case checks the two agreements at startup.
    localparam int unsigned SQ_OFF_DATA_VALID = 0,
    localparam int unsigned SQ_OFF_ADDR_VALID = SQ_OFF_DATA_VALID + 1,
    localparam int unsigned SQ_OFF_DATA       = SQ_OFF_ADDR_VALID + 1,
    localparam int unsigned SQ_OFF_SIZE       = SQ_OFF_DATA + XLEN,
    localparam int unsigned SQ_OFF_IMM        = SQ_OFF_SIZE + 3,
    localparam int unsigned SQ_OFF_BASE       = SQ_OFF_IMM + XLEN,
    localparam int unsigned SQ_OFF_ID         = SQ_OFF_BASE + XLEN,
    localparam int unsigned SQ_ENTRY_W        = SQ_OFF_ID + ID_W,

    // This module's own observation layout. The identity is the top field,
    // matching mosaic_result_fifo, mosaic_iq and mosaic_store_queue.
    localparam int unsigned OFF_SIZE   = 0,
    localparam int unsigned OFF_SIGNED = OFF_SIZE + 3,
    localparam int unsigned OFF_IMM    = OFF_SIGNED + 1,
    localparam int unsigned OFF_BASE   = OFF_IMM + XLEN,
    localparam int unsigned OFF_ID     = OFF_BASE + XLEN,
    localparam int unsigned ENTRY_W    = OFF_ID + ID_W
) (
    input  logic                          clk,
    input  logic                          rst,

    // ------------------------------------------------------------- allocation
    // A load enters the queue here, in program order. Its address operands are
    // carried as base and immediate, like the store queue's, so the memory
    // endpoint owns the one adder that turns them into an address.
    input  logic                          alloc_valid_i,
    output logic                          alloc_ready_o,
    input  mosaic_uop_pkg::uop_id_t       alloc_id_i,
    input  logic [XLEN-1:0]               alloc_base_i,
    input  logic [XLEN-1:0]               alloc_imm_i,
    input  logic [2:0]                    alloc_size_i,
    input  logic                          alloc_signed_i,

    // ------------------------------------------- the store queue's entry view
    // Every position of mosaic_store_queue, oldest first, zero beyond `o_count`.
    // See the header: this carries the program-order information the forwarding
    // query does not, and it is what makes "youngest *older*" expressible.
    input  logic [SQ_ENTRIES*SQ_ENTRY_W-1:0] sq_entry_pay_i,
    input  logic [SQ_CNT_W-1:0]              sq_count_i,

    // ---------------------------------------- the store queue's fwd query (I-034)
    // Driven by this module, consumed for the unknown-address policy.
    output logic [XLEN-1:0]               sq_query_addr_o,
    output logic [2:0]                    sq_query_size_o,
    input  logic                          sq_query_valid_i,
    input  logic [XLEN-1:0]               sq_query_data_i,
    input  logic                          sq_query_blocked_i,

    // ------------------------------------------------------- memory endpoint
    // A load request to mosaic_lsu_endpoint's upstream port.
    output logic                          req_valid_o,
    input  logic                          req_ready_i,
    output mosaic_uop_pkg::lsu_req_t      req_o,
    input  logic                          rsp_valid_i,
    output logic                          rsp_ready_o,
    input  mosaic_uop_pkg::lsu_rsp_t      rsp_i,

    // ---------------------------------------------------------------- result
    // The merged, extended load value and the per-byte source indication. The
    // result is held until accepted, so a busy consumer costs cycles, never a
    // result.
    output logic                          result_valid_o,
    input  logic                          result_ready_i,
    output mosaic_uop_pkg::uop_id_t       result_id_o,
    output logic [XLEN-1:0]               result_data_o,
    output logic [XLEN-1:0]               result_cause_o,
    output logic [XLEN-1:0]               result_tval_o,
    output logic                          result_fault_o,
    output logic [BYTES-1:0]              result_fwd_mask_o,

    // ---------------------------------------------------------- observability
    output logic [CNT_W-1:0]              o_count,
    output logic [ENTRIES-1:0]            o_occ,
    output logic [31:0]                   o_alloc_ctr,
    output logic [31:0]                   o_issue_ctr,
    output logic [31:0]                   o_rsp_ctr,
    output logic [31:0]                   o_done_ctr,
    output logic [31:0]                   o_replay_ctr,
    output logic [31:0]                   o_blocked_ctr,
    output logic [31:0]                   o_fwd_byte_ctr,
    output logic [31:0]                   o_mem_byte_ctr,
    output logic [31:0]                   o_fault_ctr,
    output logic [31:0]                   o_query_mismatch_ctr,
    // The last fault reported, so a test can compare the cause/tval pair that
    // actually reached the result port.
    output logic [XLEN-1:0]               o_last_fault_cause,
    output logic [XLEN-1:0]               o_last_fault_tval,
    output logic [ENTRIES*ENTRY_W-1:0]    o_entry_pay
);

  // ------------------------------------------------------------ elaboration
  // A queue of one cannot hold a load behind a store, so the youngest-older rule
  // would be vacuous; a store queue of one cannot hold the two stores a load can
  // span. An elaboration error rather than a comment, so a profile that made the
  // case vacuous cannot ship as a silent pass.
  localparam bit TOO_FEW_ENTRIES = (ENTRIES < 2) || (SQ_ENTRIES < 2);

  generate
    if (TOO_FEW_ENTRIES) begin : g_bad_entries
      mosaic_load_queue_contract_violation u_entries();
    end
  endgenerate

  // ------------------------------------------------------------- mutant knobs
  // One bit each so the shipping expression is one expression rather than a copy
  // per mutant. All are 1 in the shipping build.
`ifdef MOSAIC_LQ_MUTANT_YOUNGER_FORWARDS
  localparam bit FWD_NEEDS_OLDER     = 1'b0;
`else
  localparam bit FWD_NEEDS_OLDER     = 1'b1;
`endif
`ifdef MOSAIC_LQ_MUTANT_IGNORE_BLOCK
  localparam bit FWD_HONOURS_BLOCK   = 1'b0;
`else
  localparam bit FWD_HONOURS_BLOCK   = 1'b1;
`endif
`ifdef MOSAIC_LQ_MUTANT_MEMORY_OVER_STORE
  localparam bit FWD_USES_STORE      = 1'b0;
`else
  localparam bit FWD_USES_STORE      = 1'b1;
`endif

  // ----------------------------------------------------------------- entry
  typedef struct packed {
    mosaic_uop_pkg::uop_id_t id;
    logic [XLEN-1:0]         base;
    logic [XLEN-1:0]         imm;
    logic [2:0]              size;
    logic                    signed_;
  } lq_entry_t;

  // The store queue entry, decoded from the exported layout.
  typedef struct packed {
    mosaic_uop_pkg::uop_id_t id;
    logic [XLEN-1:0]         base;
    logic [XLEN-1:0]         imm;
    logic [2:0]              size;
    logic [XLEN-1:0]         data;
    logic                    addr_valid;
    logic                    data_valid;
  } sq_view_t;

  // ------------------------------------------------------------------ state
  // Entry storage is data and is deliberately not reset: validity lives in
  // `count_q`, there is no per-entry valid bit to keep in step with it, and a
  // position at or above `count_q` is masked to zero on the observation port.
  lq_entry_t        ent_q [0:ENTRIES-1];
  logic [CNT_W-1:0] count_q;      // loads held

  // The head's request is in the endpoint and has not been answered yet. One bit
  // is enough because the endpoint holds one transaction and this queue issues
  // position 0 only.
  logic inflight_q;

  // The result being held for the consumer.
  mosaic_uop_pkg::uop_id_t result_id_q;
  logic [XLEN-1:0]         result_data_q;
  logic [XLEN-1:0]         result_cause_q;
  logic [XLEN-1:0]         result_tval_q;
  logic                    result_fault_q;
  logic [BYTES-1:0]        result_fwd_mask_q;
  logic                    result_valid_q;

  // Counters
  logic [31:0] alloc_ctr_q, issue_ctr_q, rsp_ctr_q, done_ctr_q;
  logic [31:0] replay_ctr_q, blocked_ctr_q, fwd_byte_ctr_q, mem_byte_ctr_q;
  logic [31:0] fault_ctr_q;
  logic [31:0] query_mismatch_ctr_q;
  logic [XLEN-1:0] last_fault_cause_q, last_fault_tval_q;

  // -------------------------------------------------------- the store view
  // Decoded once; every consumer below reads these. Positions at or above
  // `sq_count_i` are not resident, and the store queue exports them as zero.
  sq_view_t sq_c [0:SQ_ENTRIES-1];
  logic     sq_resident_c [0:SQ_ENTRIES-1];
  logic [XLEN-1:0] sq_addr_c [0:SQ_ENTRIES-1];

  always_comb begin
    for (int unsigned j = 0; j < SQ_ENTRIES; j++) begin
      sq_c[j].data_valid = sq_entry_pay_i[j*SQ_ENTRY_W + SQ_OFF_DATA_VALID +: 1];
      sq_c[j].addr_valid = sq_entry_pay_i[j*SQ_ENTRY_W + SQ_OFF_ADDR_VALID +: 1];
      sq_c[j].data       = sq_entry_pay_i[j*SQ_ENTRY_W + SQ_OFF_DATA       +: XLEN];
      sq_c[j].size       = sq_entry_pay_i[j*SQ_ENTRY_W + SQ_OFF_SIZE       +: 3];
      sq_c[j].imm        = sq_entry_pay_i[j*SQ_ENTRY_W + SQ_OFF_IMM        +: XLEN];
      sq_c[j].base       = sq_entry_pay_i[j*SQ_ENTRY_W + SQ_OFF_BASE       +: XLEN];
      sq_c[j].id         = sq_entry_pay_i[j*SQ_ENTRY_W + SQ_OFF_ID         +: ID_W];
      sq_resident_c[j]   = (SQ_CNT_W'(j) < sq_count_i);
      sq_addr_c[j]       = sq_c[j].base + sq_c[j].imm;
    end
  end

  // ------------------------------------------------------------ the head load
  logic             head_present_c;
  lq_entry_t        head_c;
  logic [XLEN-1:0]  head_addr_c;
  logic [3:0]       head_bytes_c;

  assign head_present_c = (count_q != {CNT_W{1'b0}});
  always_comb begin
    head_c      = ent_q[0];
    head_addr_c = ent_q[0].base + ent_q[0].imm;
    head_bytes_c = 4'(mosaic_uop_pkg::size_bytes(ent_q[0].size));
  end

  // ------------------------------------------------------- the youngest-older rule
  // `FwdOlder`: the store entry's macro precedes the load's in program order.
  // The generation advances once per ROB allocation, so the half-modulus
  // distance is exact while at most ROB_ENTRIES identities are live -- the
  // contract mosaic_id_pkg's generator proves. Two uops of one macro fall back
  // to `uop_index`, which is the only order that exists inside a macro.
  function automatic logic FwdOlder(input logic [ROB_GEN_W-1:0] e_gen, input logic [UOP_INDEX_W-1:0] e_uop, input logic [ROB_GEN_W-1:0] l_gen, input logic [UOP_INDEX_W-1:0] l_uop);
    logic [ROB_GEN_W-1:0] gap;
    begin
      if (e_gen == l_gen) begin
        FwdOlder = (e_uop < l_uop);
      end else begin
        gap      = l_gen - e_gen;
        FwdOlder = (gap != {ROB_GEN_W{1'b0}}) && !gap[ROB_GEN_W-1];
      end
    end
  endfunction

  // `FwdCovers`: the store entry's byte range contains address `x`. Byte
  // granularity is the point: `FWD_BYTE_RANGE` is what
  // MOSAIC_LQ_MUTANT_ALIGNED_WORD_ONLY removes, and the difference is exactly a
  // partially overlapping parallel access -- which only a word-aligned compare
  // misses.
  function automatic logic FwdCovers(input logic [XLEN-1:0] e_addr,
                                     input logic [2:0]      e_size,
                                     input logic [XLEN-1:0] x);
    logic [XLEN-1:0] e_end;
    begin
      e_end = e_addr + mosaic_uop_pkg::size_bytes(e_size);
`ifdef MOSAIC_LQ_MUTANT_ALIGNED_WORD_ONLY
      FwdCovers = (e_addr[XLEN-1:3] == x[XLEN-1:3]);
`else
      FwdCovers = (x >= e_addr) && (x < e_end);
`endif
    end
  endfunction

  // ------------------------------------------------------- per-byte selection
  // For each load byte, the source is the youngest older covering store with
  // captured data; `src_nodata` records that the youngest older covering store
  // has no data yet, which blocks the load rather than letting it fall back.
  logic [BYTES-1:0]      src_store_c;    // a store provides this byte
  logic [BYTES-1:0]      src_nodata_c;   // the governing store has no data yet
  logic [XLEN-1:0]       src_byte_c  [0:BYTES-1];
  logic [XLEN-1:0]       sel_x_c;        // the load byte's address, per iteration
  logic [XLEN-1:0]       sel_off_c;      // its offset inside the matched store

  always_comb begin
    for (int unsigned i = 0; i < BYTES; i++) begin
      sel_x_c = head_addr_c + XLEN'(i);
      src_store_c[i]  = 1'b0;
      src_nodata_c[i] = 1'b0;
      src_byte_c[i]   = {XLEN{1'b0}};
      // Oldest first, so the last match is the youngest.
      for (int unsigned j = 0; j < SQ_ENTRIES; j++) begin
        logic older;
        logic covers;
        older  = !FWD_NEEDS_OLDER ||
                 FwdOlder(sq_c[j].id.rob_gen, sq_c[j].id.uop_index,
                          head_c.id.rob_gen, head_c.id.uop_index);
        covers = FwdCovers(sq_addr_c[j], sq_c[j].size, sel_x_c);
        sel_off_c = sel_x_c - sq_addr_c[j];
        if (sq_resident_c[j] && older && sq_c[j].addr_valid && covers) begin
          if (sq_c[j].data_valid) begin
            src_store_c[i]  = 1'b1;
            src_nodata_c[i] = 1'b0;
            src_byte_c[i]   = (sq_c[j].data >> {sel_off_c[2:0], 3'b000}) & 64'hff;
          end else begin
            src_store_c[i]  = 1'b0;
            src_nodata_c[i] = 1'b1;
          end
        end
      end
      // Bytes outside the load's own size are never sourced.
      if (i >= head_bytes_c) begin
        src_store_c[i]  = 1'b0;
        src_nodata_c[i] = 1'b0;
      end
    end
  end

  // ------------------------------------------------------------- the block
  // The store queue's unknown-address flag is the card's policy and is taken
  // whole. `src_nodata_c` adds the "address known, data not captured" wait that
  // the flag does not express.
  logic older_nodata_c;
  always_comb begin
    older_nodata_c = 1'b0;
    for (int unsigned i = 0; i < BYTES; i++) begin
      if (src_nodata_c[i]) older_nodata_c = 1'b1;
    end
  end

  logic blocked_c;
  assign blocked_c = (FWD_HONOURS_BLOCK && sq_query_blocked_i) || older_nodata_c;

  // ------------------------------------------------------------- the merge
  // Byte `i` of the load is the store's byte when an older store provides it,
  // and the memory byte otherwise. The memory byte comes from the endpoint's
  // own response: its low bytes are the memory bytes at
  // `load_addr .. load_addr+n-1`, in this same order.
  logic [XLEN-1:0] merged_c;
  logic [BYTES-1:0] fwd_mask_c;

  always_comb begin
    merged_c   = {XLEN{1'b0}};
    fwd_mask_c = {BYTES{1'b0}};
    for (int unsigned i = 0; i < BYTES; i++) begin
      if (i < head_bytes_c) begin
        fwd_mask_c[i] = src_store_c[i] && FWD_USES_STORE;
        if (fwd_mask_c[i]) begin
          merged_c[i*8 +: 8] = src_byte_c[i][7:0];
        end else begin
          merged_c[i*8 +: 8] = rsp_i.data[i*8 +: 8];
        end
      end
    end
  end

  // ------------------------------------------------------------- extension
  // The merged word is extended by size exactly as the endpoint extends a pure
  // memory value; the merge does not change the architectural value's type.
  logic [XLEN-1:0] extended_c;
  always_comb begin
    case (head_c.size)
      mosaic_pkg::SZ_BYTE: begin
        extended_c = head_c.signed_ ? {{56{merged_c[7]}},  merged_c[7:0]}
                                    : {56'd0, merged_c[7:0]};
      end
      mosaic_pkg::SZ_HALF: begin
        extended_c = head_c.signed_ ? {{48{merged_c[15]}}, merged_c[15:0]}
                                    : {48'd0, merged_c[15:0]};
      end
      mosaic_pkg::SZ_WORD: begin
        extended_c = head_c.signed_ ? {{32{merged_c[31]}}, merged_c[31:0]}
                                    : {32'd0, merged_c[31:0]};
      end
      default: begin
        extended_c = merged_c;
      end
    endcase
  end

  // --------------------------------------------------- the stale-forward knob
  // MOSAIC_LQ_MUTANT_STALE_FORWARD latches the forwarded bytes when the load is
  // issued and reuses them at completion, instead of reading the store queue as
  // it stands when the response arrives. A store whose address resolves in
  // between is then missed.
`ifdef MOSAIC_LQ_MUTANT_STALE_FORWARD
  logic [BYTES-1:0] issued_mask_q;
  logic [XLEN-1:0]  issued_bytes_q;

  logic [BYTES-1:0] final_mask_c;
  logic [XLEN-1:0]  final_merged_c;
  always_comb begin
    final_mask_c   = issued_mask_q;
    final_merged_c = issued_bytes_q;
    for (int unsigned i = 0; i < BYTES; i++) begin
      if (i < head_bytes_c && !issued_mask_q[i]) begin
        final_merged_c[i*8 +: 8] = rsp_i.data[i*8 +: 8];
      end
    end
  end

  logic [XLEN-1:0] final_ext_c;
  always_comb begin
    case (head_c.size)
      mosaic_pkg::SZ_BYTE: begin
        final_ext_c = head_c.signed_ ? {{56{final_merged_c[7]}},  final_merged_c[7:0]}
                                     : {56'd0, final_merged_c[7:0]};
      end
      mosaic_pkg::SZ_HALF: begin
        final_ext_c = head_c.signed_ ? {{48{final_merged_c[15]}}, final_merged_c[15:0]}
                                     : {48'd0, final_merged_c[15:0]};
      end
      mosaic_pkg::SZ_WORD: begin
        final_ext_c = head_c.signed_ ? {{32{final_merged_c[31]}}, final_merged_c[31:0]}
                                     : {32'd0, final_merged_c[31:0]};
      end
      default: begin
        final_ext_c = final_merged_c;
      end
    endcase
  end
`else
  logic [BYTES-1:0] final_mask_c;
  logic [XLEN-1:0]  final_ext_c;
  assign final_mask_c = fwd_mask_c;
  assign final_ext_c  = extended_c;
`endif

  // ---------------------------------------------------------------- request
  // The head is offered while nothing is in flight and no result is pending.
  // Gating on the result register is the in-order handoff: one result at a time,
  // in program order, with no reorder buffer between them.
  assign req_valid_o = head_present_c && !inflight_q && !result_valid_q;
  assign rsp_ready_o = 1'b1;

  always_comb begin
    req_o.id         = head_c.id;
    req_o.we         = 1'b0;
    req_o.base       = head_c.base;
    req_o.imm        = head_c.imm;
    req_o.size       = head_c.size;
    req_o.signed_    = head_c.signed_;
    req_o.store_data = {XLEN{1'b0}};
  end

  logic issue_accept_c;
  assign issue_accept_c = req_valid_o && req_ready_i;
  logic rsp_accept_c;
  assign rsp_accept_c = rsp_valid_i && rsp_ready_o;
  logic result_accept_c;
  assign result_accept_c = result_valid_q && result_ready_i;

  // ------------------------------------------------------------- query drive
  // The store queue is asked about the load at the head, every cycle, so the
  // unknown-address policy is taken from the packet the store queue computes
  // rather than from a second copy of it here.
  assign sq_query_addr_o = head_addr_c;
  assign sq_query_size_o = head_c.size;

  // --------------------------------------------------------- the completion
  // A response completes the head unless a store's address or data is still
  // missing. A blocked load is replayed rather than completed: the endpoint's
  // response is discarded and the load re-issued once the block lifts, so the
  // memory value it merges is the one read after every relevant older store was
  // known -- the card's "waits for all relevant older store addresses" policy.
  logic complete_c;
  logic replay_c;
  assign complete_c = inflight_q && rsp_accept_c && !blocked_c;
  assign replay_c   = inflight_q && rsp_accept_c && blocked_c;

  // ------------------------------------------------------------- next state
  lq_entry_t         ent_next_c [0:ENTRIES-1];
  logic [CNT_W-1:0]  next_count_c;

  always_comb begin
    for (int unsigned i = 0; i < ENTRIES; i++) begin
      ent_next_c[i] = ent_q[i];
    end
    next_count_c = count_q;
    // A completion removes position 0 and the survivors shift down; an
    // allocation is appended at the current occupancy. Both happen in the same
    // always_comb so a completion and an allocation in one cycle cannot disagree
    // about the occupancy.
    if (complete_c && head_present_c) begin
      for (int unsigned i = 0; i < ENTRIES-1; i++) begin
        ent_next_c[i] = ent_q[i+1];
      end
      next_count_c = count_q - CNT_W'(1);
    end
    if (alloc_valid_i && alloc_ready_o) begin
      ent_next_c[next_count_c[IDX_W-1:0]].id      = alloc_id_i;
      ent_next_c[next_count_c[IDX_W-1:0]].base    = alloc_base_i;
      ent_next_c[next_count_c[IDX_W-1:0]].imm     = alloc_imm_i;
      ent_next_c[next_count_c[IDX_W-1:0]].size    = alloc_size_i;
      ent_next_c[next_count_c[IDX_W-1:0]].signed_ = alloc_signed_i;
      next_count_c = next_count_c + CNT_W'(1);
    end
  end

  assign alloc_ready_o = (count_q < CNT_W'(ENTRIES));

  // ------------------------------------------------------------- the edge
  always_ff @(posedge clk) begin
    if (rst) begin
      count_q           <= {CNT_W{1'b0}};
      inflight_q        <= 1'b0;
      result_valid_q    <= 1'b0;
      result_id_q       <= {ID_W{1'b0}};
      result_data_q     <= {XLEN{1'b0}};
      result_cause_q    <= {XLEN{1'b0}};
      result_tval_q     <= {XLEN{1'b0}};
      result_fault_q    <= 1'b0;
      result_fwd_mask_q <= {BYTES{1'b0}};
      alloc_ctr_q       <= 32'd0;
      issue_ctr_q       <= 32'd0;
      rsp_ctr_q         <= 32'd0;
      done_ctr_q        <= 32'd0;
      replay_ctr_q      <= 32'd0;
      blocked_ctr_q     <= 32'd0;
      fault_ctr_q       <= 32'd0;
      last_fault_cause_q <= {XLEN{1'b0}};
      last_fault_tval_q  <= {XLEN{1'b0}};
`ifdef MOSAIC_LQ_MUTANT_STALE_FORWARD
      issued_mask_q     <= {BYTES{1'b0}};
      issued_bytes_q    <= {XLEN{1'b0}};
`endif
    end else begin
      for (int unsigned i = 0; i < ENTRIES; i++) begin
        ent_q[i] <= ent_next_c[i];
      end
      count_q <= next_count_c;

      if (alloc_valid_i && alloc_ready_o) alloc_ctr_q <= alloc_ctr_q + 32'd1;
      if (issue_accept_c) begin
        issue_ctr_q <= issue_ctr_q + 32'd1;
        inflight_q  <= 1'b1;
`ifdef MOSAIC_LQ_MUTANT_STALE_FORWARD
        issued_mask_q  <= fwd_mask_c;
        issued_bytes_q <= merged_c;
`endif
      end
      if (rsp_accept_c) begin
        rsp_ctr_q   <= rsp_ctr_q + 32'd1;
        inflight_q  <= 1'b0;
        if (complete_c) begin
          result_valid_q    <= 1'b1;
          result_id_q       <= head_c.id;
          result_data_q     <= rsp_i.fault ? rsp_i.data : final_ext_c;
          result_cause_q    <= rsp_i.cause;
          result_tval_q     <= rsp_i.tval;
          result_fault_q    <= rsp_i.fault;
          result_fwd_mask_q <= rsp_i.fault ? {BYTES{1'b0}} : final_mask_c;
          done_ctr_q        <= done_ctr_q + 32'd1;
        end
        if (replay_c) replay_ctr_q <= replay_ctr_q + 32'd1;
        if (blocked_c) blocked_ctr_q <= blocked_ctr_q + 32'd1;
        if (rsp_i.fault) begin
          fault_ctr_q        <= fault_ctr_q + 32'd1;
          last_fault_cause_q <= rsp_i.cause;
          last_fault_tval_q  <= rsp_i.tval;
        end
      end
      if (result_accept_c) result_valid_q <= 1'b0;
    end
  end

  // ------------------------------------------------------------ byte counters
  // Counted combinationally so a mutant that changes the merge changes them
  // too; they are observations, not state the datapath reads.
  logic [CNT_W-1:0] fwd_byte_cnt_c;
  logic [CNT_W-1:0] mem_byte_cnt_c;
  always_comb begin
    fwd_byte_cnt_c = {CNT_W{1'b0}};
    mem_byte_cnt_c = {CNT_W{1'b0}};
    for (int unsigned i = 0; i < BYTES; i++) begin
      if (i < head_bytes_c) begin
        if (final_mask_c[i]) fwd_byte_cnt_c = fwd_byte_cnt_c + CNT_W'(1);
        else                 mem_byte_cnt_c = mem_byte_cnt_c + CNT_W'(1);
      end
    end
  end

  // ------------------------------------------- the store queue query cross-check
  // The query's rule ("the youngest resident entry that fully contains the
  // queried range, in the doubleword the address selects") applied here to the
  // entries the store queue exports. It is deliberately independent of the
  // mutant knobs -- it is the *store queue's* rule, not this module's search --
  // so a disagreement means the two views of the same fact have diverged.
  logic [3:0] q_bytes_c;
  assign q_bytes_c = 4'(mosaic_uop_pkg::size_bytes(head_c.size));

  function automatic logic QueryCovers(input logic [XLEN-1:0] e_addr, input logic [2:0] e_size);
    logic [3:0] e_lo, q_lo, e_n;
    begin
      e_lo = 4'(e_addr[2:0]);
      q_lo = 4'(head_addr_c[2:0]);
      e_n  = 4'(mosaic_uop_pkg::size_bytes(e_size));
      QueryCovers = (e_addr[XLEN-1:3] == head_addr_c[XLEN-1:3]) &&
                    (e_lo <= q_lo) && ((q_lo + q_bytes_c) <= (e_lo + e_n));
    end
  endfunction

  logic            q_valid_c;
  logic [XLEN-1:0] q_data_c;
  logic            q_blocked_c;
  logic [2:0]      q_lane_c;
  always_comb begin
    q_valid_c   = 1'b0;
    q_data_c    = {XLEN{1'b0}};
    q_blocked_c = 1'b0;
    q_lane_c    = 3'd0;
    for (int unsigned j = 0; j < SQ_ENTRIES; j++) begin
      if (sq_resident_c[j] && !sq_c[j].addr_valid) q_blocked_c = 1'b1;
      if (sq_resident_c[j] && sq_c[j].addr_valid && sq_c[j].data_valid &&
          QueryCovers(sq_addr_c[j], sq_c[j].size)) begin
        q_valid_c = 1'b1;
        q_lane_c  = sq_addr_c[j][2:0];
        q_data_c  = sq_c[j].data << {q_lane_c, 3'b000};
      end
    end
  end

  logic q_mismatch_c;
  assign q_mismatch_c = (sq_query_valid_i != q_valid_c) ||
                        (sq_query_blocked_i != q_blocked_c) ||
                        (q_valid_c && (sq_query_data_i != q_data_c));

  always_ff @(posedge clk) begin
    if (rst) begin
      fwd_byte_ctr_q     <= 32'd0;
      mem_byte_ctr_q     <= 32'd0;
      query_mismatch_ctr_q <= 32'd0;
    end else begin
      if (complete_c && !rsp_i.fault) begin
        fwd_byte_ctr_q <= fwd_byte_ctr_q + 32'(fwd_byte_cnt_c);
        mem_byte_ctr_q <= mem_byte_ctr_q + 32'(mem_byte_cnt_c);
      end
      if (q_mismatch_c) query_mismatch_ctr_q <= query_mismatch_ctr_q + 32'd1;
    end
  end

  // ------------------------------------------------------------- outputs
  assign result_valid_o    = result_valid_q;
  assign result_id_o       = result_id_q;
  assign result_data_o     = result_data_q;
  assign result_cause_o    = result_cause_q;
  assign result_tval_o     = result_tval_q;
  assign result_fault_o    = result_fault_q;
  assign result_fwd_mask_o = result_fwd_mask_q;

  assign o_count              = count_q;
  assign o_alloc_ctr          = alloc_ctr_q;
  assign o_issue_ctr          = issue_ctr_q;
  assign o_rsp_ctr            = rsp_ctr_q;
  assign o_done_ctr           = done_ctr_q;
  assign o_replay_ctr         = replay_ctr_q;
  assign o_blocked_ctr        = blocked_ctr_q;
  assign o_fwd_byte_ctr       = fwd_byte_ctr_q;
  assign o_mem_byte_ctr       = mem_byte_ctr_q;
  assign o_fault_ctr          = fault_ctr_q;
  assign o_query_mismatch_ctr = query_mismatch_ctr_q;
  assign o_last_fault_cause   = last_fault_cause_q;
  assign o_last_fault_tval    = last_fault_tval_q;

  always_comb begin
    for (int unsigned i = 0; i < ENTRIES; i++) begin
      o_occ[i] = (CNT_W'(i) < count_q);
    end
  end

  always_comb begin
    o_entry_pay = {ENTRIES*ENTRY_W{1'b0}};
    for (int unsigned i = 0; i < ENTRIES; i++) begin
      if (CNT_W'(i) < count_q) begin
        o_entry_pay[i*ENTRY_W + OFF_SIZE   +: 3]    = ent_q[i].size;
        o_entry_pay[i*ENTRY_W + OFF_SIGNED +: 1]    = ent_q[i].signed_;
        o_entry_pay[i*ENTRY_W + OFF_IMM    +: XLEN] = ent_q[i].imm;
        o_entry_pay[i*ENTRY_W + OFF_BASE   +: XLEN] = ent_q[i].base;
        o_entry_pay[i*ENTRY_W + OFF_ID     +: ID_W] = ent_q[i].id;
      end
    end
  end

endmodule : mosaic_load_queue

`resetall
`default_nettype wire

`endif  // MOSAIC_LOAD_QUEUE_SV_
