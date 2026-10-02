// ============================================================================
// mosaic_store_queue -- work package I-034: the speculative store queue and
// store-commit authorisation.
//
// A store is the only instruction whose side effect leaves the hart, and it is
// the only side effect that cannot be undone. Everything in this module follows
// from one sentence:
//
//   A store may reach memory only when (a) it is authorised to, in program
//   order, because it retired, and (b) it is non-faulting.
//
// The defect this module exists to prevent is the tempting simplification
// "the store's address and data are ready, so it can go now". A store executed
// on a speculated path has an address and data just as ready as a correct one;
// what it does not have is a commit. Execution-complete is an *observation*,
// commit is an *authorisation*, and this queue is the place that keeps them
// apart -- every entry records that its address and data were captured, and the
// queue as a whole records how many of them retired. Nothing is offered to the
// memory side until both are true.
//
// ------------------------------------------------------- what the entry holds
//
// Address capture and data capture are separate facts, and the entry stores
// them as two independent validity bits:
//
//     | field      | when it is captured                                        |
//     |------------|------------------------------------------------------------|
//     | base, imm  | `addr_valid`: both address operands are available           |
//     | data       | `data_valid`: the store operand is available                |
//
// Neither is required at allocation. A store whose operand is not ready is
// allocated with `data_valid=0` and filled later (`fill_valid_i`); one whose
// address operands are not ready is allocated with `addr_valid=0`. The two
// arrivals are unordered with respect to each other and to the commit: an entry
// becomes drainable only when it is authorised **and** both bits are set, so a
// store whose address is unknown when its data is ready is held, not leaked.
//
// The two validity bits are per entry and are not implicit in "the entry
// exists", for the same reason mosaic_result_fifo keeps the exception bit next
// to the data: the queue must be able to hold captured-address, captured-data,
// authorised and drained as four independent states of one entry, and a design
// that merges any two of them can only represent three of the sixteen states it
// needs.
//
// ------------------------------------------------------ order and authorisation
//
// The queue is an **ordered compacted sequence**, position 0 oldest, exactly
// the shape mosaic_result_fifo documents: a drain removes position 0 and the
// survivors shift down, a squash removes a suffix and the survivors shift down,
// and a new allocation is appended at the current occupancy. There is no
// circular pointer pair, so the "same-cycle pop frees the head slot while a push
// fills the tail slot" aliasing bug is unrepresentable.
//
// Authorisation is an **in-order watermark**, `auth_cnt`: entries
// `0 .. auth_cnt-1` are authorised and no others. That is the whole reason the
// watermark rather than a per-entry bit is the right representation:
//
//   * commit is in order, so the set of authorised entries is always a prefix;
//   * a prefix cannot express "the third store committed but the first did
//     not", which is exactly the state that must not exist;
//   * the drain takes position 0, so an unauthorised older store is a wall that
//     no younger store can pass -- structurally, not by a comparison.
//
// `commit_valid_i` carries the identity of the store the ROB is committing, and
// the authorisation is applied only when that identity names the first
// unauthorised entry. An authorisation that names anything else (an entry that
// is not resident, or one that is not next) is *refused* and counted in
// `o_commit_stale_ctr`; it authorises nothing. Refusing is the safe direction,
// and it is what makes "a store behind an unauthorised one does not bypass it"
// a checked property rather than an assumption about the caller.
//
// `commit2_valid_i` is the same rule applied to the *second* store the ROB
// retires in one cycle (MOSAIC_RETIRE_WIDTH is 2), naming the entry after the
// first. It exists because a store that retires must be authorised in that
// cycle or the machine can enter the one state the squash rule cannot handle:
// retired, resident and unauthorised. Such an entry is too old for the dead
// window to take back (it has retired) and, being unauthorised, is not spared
// by `SQUASH_SPARES_AUTHORISED` -- so a whole-queue flush would silently delete
// a store that the architecture has already promised. Two stores retiring
// together is the common case for back-to-back stores, not a corner.
//
// `commit_valid_i` and `squash_valid_i` are not expected in the same cycle: the
// ROB does not retire in a flush cycle (`mosaic_rob`: `retire_ack = retire_req &&
// head_ready && !flush_valid`), and a squash kills younger work than anything
// that retired. The queue nevertheless behaves coherently if they do arrive
// together, because the watermark is *derived from the survivors* rather than
// adjusted by arithmetic: it counts the leading entries that are both resident
// and authorised after this cycle's drain, squash and commit, so a commit whose
// entry the same cycle's squash takes cannot leave the watermark pointing past
// the entry it counted.
//
// ------------------------------------------------------------ the squash rule
//
// A squash withdraws the speculative work the recovery decided is dead. The
// rule is a region of the ROB index space, because that is the only ordering
// both this queue and the ROB hold: mosaic_recovery publishes
// `o_rob_flush_from` (the ROB index of the first instruction to discard) and
// `rob_flush_valid`, and the ROB's allocation pointer bounds the young window
// (mosaic_rob does not expose it yet; the port is declared and the gap is named
// here rather than papered over with a whole-queue flush). An entry dies when
//
//     squash_valid_i
//       && ( squash_all_i
//            || ( entry.rob_gen == squash_gen_i
//                 && ((entry.rob_index - squash_from_index_i) mod 2**ROB_INDEX_W)
//                     < ((squash_tail_index_i - squash_from_index_i) mod 2**ROB_INDEX_W) ) )
//       && !entry_is_authorised
//
// The generation comparison is not decoration. A store that survived an earlier
// squash keeps its identity, and after the ROB has wrapped and re-flushed, an
// index-only window would match it by accident; the generation is what makes a
// recycled slot a *different* instruction. Both distances are computed modulo
// the ROB index space, so a window that crosses the wrap point is as valid as
// one that does not.
//
// The last conjunct is the card's second Fail criterion: **a flush must not
// delete a store that was already externally promised.** A store that reached
// `auth_cnt` has retired; it is past the point where recovery may take it back.
// It is therefore spared -- and because a *full* flush (`squash_all_i`, which is
// what mosaic_rob's `flush_valid` is today) would otherwise sweep it away, this
// is not a corner case: an authorised store that has not yet drained is
// resident across a full flush by construction. `o_squash_spared_ctr` counts the
// entries that the region would have taken and that the watermark spared, so
// the behaviour is observed on every run rather than argued.
//
// ------------------------------------------------ the drain, and exactly once
//
// Position 0 is offered to the memory side through mosaic_lsu_endpoint's
// upstream port when it is authorised and both its address and its data are
// captured. The endpoint is the authority on "non-faulting": it traps a
// misaligned access before the memory sees it and reports a memory access fault
// from the response, and both arrive here as an `lsu_rsp_t` with `fault` set. A
// faulting store therefore writes nothing (the endpoint's own contract, checked
// by its own case), and this module counts it in `o_fault_ctr` and records its
// identity.
//
// The entry is removed from the queue in the cycle the **request** is accepted,
// not the cycle the response arrives. That is the exactly-once rule: the
// endpoint owns the transaction from acceptance, only the queue's own state can
// re-issue it, and there is no state here in which the same entry can be offered
// twice -- `drain_removes_c` is on the same edge as the offer's acceptance, and
// the response is consumed unconditionally (`drain_rsp_ready_o` is tied high)
// because there is nothing left for it to do but be tallied.
//
// The offer is *withdrawn* in the cycle a squash kills the offered entry (the
// same rule mosaic_iq applies to a grant: "a squashed uop must not issue"), so a
// store killed before it is accepted never reaches the memory side. The payload
// is stable in every cycle `drain_req_valid_o` is high; a withdrawal is a
// falling valid, and the entry behind it is a new offer, not a changed one.
//
// ------------------------------------------------------------- conservation
//
//     o_alloc_ctr == o_drain_ctr + o_squash_ctr + o_count
//
// Every accepted allocation leaves exactly once: drained, squashed, or still
// resident. A lost allocation, a duplicate drain and a dropped squash each break
// it by exactly their own count, which is what makes an allocation accepted and
// silently discarded (a full queue that says it is ready) a named failure rather
// than a missing store nobody notices until it does not write memory.
//
// `alloc_ready_o` is a function of the registered occupancy and nothing else --
// `o_count < ENTRIES` -- so there is no combinational path from the memory side
// or from the drain back into the allocator, the trade mosaic_result_fifo and
// mosaic_fifo both document. A drain that happens in the same cycle frees its
// slot at the edge, not for that cycle, and a refused allocation is offered
// again unchanged (the transport rule holds the payload while `valid && !ready`).
//
// -------------------------------------------------- store-to-load forwarding
//
// The card leaves one interface decision to this package: how I-035's younger
// load asks "is there a store ahead of me whose data I can use?". The decision
// is **a purely combinational query port with no handshake and no side effects**:
//
//     fwd_query_addr_i, fwd_query_size_i  ->  fwd_valid_o, fwd_data_o, fwd_blocked_o
//
// Its rule, stated once:
//
//   * The answer is the **youngest** resident entry (the highest position, since
//     the queue is in program order) that has both address and data captured and
//     whose byte range, in the doubleword the address selects, **fully contains**
//     the queried byte range.
//   * `fwd_data_o` is that entry's data in the *memory-response lane convention*
//     (`mosaic_uop_pkg::lsu_rsp_t.data`, "the byte at `addr` is lane `addr[2:0]`"),
//     i.e. the store payload aligned to the store's own address. A load extracts
//     its bytes from it exactly as it extracts them from memory.
//   * `fwd_blocked_o` is high when any resident entry has no address yet. Such a
//     store might be older than the load and might overlap it, so the answer may
//     be overturned; I-035/I-036 must treat the answer as advisory while it is
//     high. (Conservative on purpose: the queue does not know where the load is
//     in program order, and a query port that guessed would be a second
//     speculation mechanism.)
//   * Partial overlap is *not* answered: a store that covers only part of the
//     query returns `fwd_valid_o = 0`, and the consumer merges byte lanes between
//     the queue and memory itself. Answering a partial store here would move the
//     byte-merge rule out of I-035, which owns it.
//
// The port is a pure function of the query inputs and the queue's registers, so
// leaving it unconnected is safe, and nothing in this module or in the case's
// endpoint depends on it. It is **exercised**: `PhaseForward` in
// sim/unit/tb_store.cpp drives the rule directly (whole-doubleword, byte, word,
// crossing-the-boundary, partial-coverage, blocked-by-unknown-address and
// youngest-wins cases), and every cycle of every phase also compares the two
// outputs against the rule. It carries no mutant: the visibility contract is
// what the case is about, and this port cannot change one.
//
// ------------------------------------------------------------- negative controls
//
// -DMOSAIC_SQ_MUTANT_<n> injects one defect the case must detect. None is
// defined in the shipping build. The table with real build commands, binary
// hashes and exit codes is in results/reports/I-034-sq.md:
//
//   VISIBLE_BEFORE_COMMIT        the drain offer ignores the authorisation
//                               watermark: an entry with a captured address and
//                               data goes as soon as it is ready, so a
//                               wrong-path store writes memory.
//   FULL_FLUSH_DROPS_AUTHORISED the squash region ignores the watermark, so a
//                               flush deletes a store that already retired and
//                               has not drained yet.
//   DRAIN_DUPLICATE             an accepted drain does not remove the entry, so
//                               the same store is offered -- and written -- again.
//   DRAIN_OUT_OF_ORDER          the drain may take any ready authorised entry
//                               instead of position 0, so a store bypasses an
//                               older one that is not authorised yet.
//   ALLOC_DROP_WHEN_FULL        `alloc_ready_o` is asserted at capacity and the
//                               entry is discarded: a silent lost store.
//   EARLY_VISIBILITY            an entry whose address or data is still unknown
//                               is offered with the bytes it happens to hold.
//   COMMIT_OUT_OF_ORDER         an authorisation naming a younger entry is
//                               applied to it, advancing the watermark past the
//                               stores that have not retired.
// ============================================================================

`ifndef MOSAIC_STORE_QUEUE_SV_
`define MOSAIC_STORE_QUEUE_SV_

`default_nettype none
`resetall

/* verilator lint_off UNUSEDPARAM */
// The generated header declares one localparam per knob for the whole project.
// This module names the LSU subset it needs; the rest belong to other modules
// and are unused *here* by construction, not by omission.
`include "mosaic_cfg_pkg.svh"
/* verilator lint_on UNUSEDPARAM */

module mosaic_store_queue #(
    // Geometry, from the generated packages only. `localparam`, not `parameter`:
    // a caller-supplied default would be a second geometry. The identity widths
    // come from mosaic_uop_pkg, which re-exports them from mosaic_id_pkg; this
    // file therefore does not include the identity header a second time (it has
    // no include guard, and a second definition of it in one compilation unit is
    // a duplicate package rather than a harmless repeat).
    localparam int unsigned ENTRIES     = mosaic_cfg_pkg::MOSAIC_SQ_ENTRIES,
    localparam int unsigned XLEN        = mosaic_cfg_pkg::MOSAIC_XLEN,
    localparam int unsigned ROB_INDEX_W = mosaic_uop_pkg::ROB_INDEX_W,
    localparam int unsigned ROB_GEN_W   = mosaic_uop_pkg::ROB_GEN_W,
    localparam int unsigned ID_W        = $bits(mosaic_uop_pkg::uop_id_t),

    // Derived, so not overridable. CNT_W can hold 0..ENTRIES: the occupancy is a
    // count, not an index, and the watermark needs the extra bit to say
    // "everything is authorised".
    localparam int unsigned CNT_W       = $clog2(ENTRIES + 1),
    localparam int unsigned IDX_W       = (ENTRIES <= 1) ? 1 : $clog2(ENTRIES),

    // Observation layout, least significant field bit first. The identity is the
    // top field, matching mosaic_result_fifo and mosaic_iq.
    localparam int unsigned OFF_DATA_VALID = 0,
    localparam int unsigned OFF_ADDR_VALID = OFF_DATA_VALID + 1,
    localparam int unsigned OFF_DATA       = OFF_ADDR_VALID + 1,
    localparam int unsigned OFF_SIZE       = OFF_DATA + XLEN,
    localparam int unsigned OFF_IMM        = OFF_SIZE + 3,
    localparam int unsigned OFF_BASE       = OFF_IMM + XLEN,
    localparam int unsigned OFF_ID         = OFF_BASE + XLEN,
    localparam int unsigned ENTRY_W        = OFF_ID + ID_W
) (
    input  logic                          clk,
    input  logic                          rst,

    // ------------------------------------------------------------- allocation
    // A store enters the queue here, in program order, whether or not its
    // address and data are ready. The two readiness bits are independent: a
    // store is allocated with the facts that exist at dispatch and the rest
    // arrives on the fill port. `alloc_ready_o` is a function of the registered
    // occupancy, so a refused allocation is offered again unchanged.
    input  logic                          alloc_valid_i,
    output logic                          alloc_ready_o,
    input  mosaic_uop_pkg::uop_id_t       alloc_id_i,
    input  logic [XLEN-1:0]               alloc_base_i,
    input  logic [XLEN-1:0]               alloc_imm_i,
    input  logic [2:0]                    alloc_size_i,
    input  logic                          alloc_addr_valid_i,
    input  logic [XLEN-1:0]               alloc_data_i,
    input  logic                          alloc_data_valid_i,

    // ---------------------------------------------------------- late operand
    // The operand that was not ready at allocation. It is matched by identity to
    // the oldest resident entry that carries it; a fill that names no resident
    // entry is counted and dropped (it is stale by definition -- the only way a
    // store leaves this queue is by draining or by being squashed).
    input  logic                          fill_valid_i,
    input  mosaic_uop_pkg::uop_id_t       fill_id_i,
    input  logic [XLEN-1:0]               fill_base_i,
    input  logic [XLEN-1:0]               fill_imm_i,
    input  logic                          fill_addr_valid_i,
    input  logic [XLEN-1:0]               fill_data_i,
    input  logic                          fill_data_valid_i,
    output logic                          fill_hit_o,
    output logic                          fill_stale_o,

    // ------------------------------------------------------ commit authorisation
    // `commit_id_i` names the store the ROB is committing. It is applied only
    // when it names the first unauthorised entry; anything else is refused and
    // counted. In order, at most one per cycle, because commit is in order.
    input  logic                          commit_valid_i,
    input  mosaic_uop_pkg::uop_id_t       commit_id_i,
    output logic                          commit_ok_o,
    output logic                          commit_stale_o,

    // The second authorisation of the same cycle. The ROB retires up to
    // MOSAIC_RETIRE_WIDTH (2) instructions per cycle and stores are the
    // instructions most likely to be back to back, so two consecutive stores
    // retire together routinely; with one port the younger of the pair would
    // retire *unauthorised*. That state is exactly the one the header says must
    // not exist: the store is too old to be squashed (it has retired) and
    // unauthorised, so a flush could neither spare it nor take it back --
    // squash_all would delete it and the store would never reach memory.
    // Authorising both retiring stores in their own retire cycle keeps
    // "retired implies authorised" exact. `commit2_id_i` names the first
    // unauthorised entry *after* the one `commit_valid_i` authorises, so the
    // pair is the same prefix rule applied twice.
    input  logic                          commit2_valid_i,
    input  mosaic_uop_pkg::uop_id_t       commit2_id_i,
    output logic                          commit2_ok_o,
    output logic                          commit2_stale_o,

    // ------------------------------------------------------------ squash/flush
    // `squash_all_i` is a whole-queue flush (mosaic_rob's `flush_valid` today).
    // Otherwise the dead region is the ROB window starting at
    // `squash_from_index_i` (mosaic_recovery's `o_rob_flush_from`) and ending at
    // `squash_tail_index_i` (the ROB's allocation pointer), in generation
    // `squash_gen_i`. See the header for the exact predicate.
    input  logic                          squash_valid_i,
    input  logic                          squash_all_i,
    input  logic [ROB_INDEX_W-1:0]        squash_from_index_i,
    input  logic [ROB_INDEX_W-1:0]        squash_tail_index_i,
    input  logic [ROB_GEN_W-1:0]          squash_gen_i,

    // ------------------------------------------------------- downstream memory
    // A store request to mosaic_lsu_endpoint, which owns the fault boundary. The
    // entry leaves the queue when the request is accepted; the response is
    // consumed unconditionally and only tallied.
    output logic                          drain_req_valid_o,
    input  logic                          drain_req_ready_i,
    output mosaic_uop_pkg::lsu_req_t      drain_req_o,
    input  logic                          drain_rsp_valid_i,
    output logic                          drain_rsp_ready_o,
    // The endpoint's response carries the fault, its cause/tval and the identity
    // of the access it belongs to; a store has no loaded value, so the `data`
    // field is unused *here* by construction rather than by omission.
    /* verilator lint_off UNUSEDSIGNAL */
    input  mosaic_uop_pkg::lsu_rsp_t      drain_rsp_i,
    /* verilator lint_on UNUSEDSIGNAL */

    // --------------------------------------------------- forwarding query (I-035)
    // Purely combinational, no handshake, no side effects. See the header for
    // the rule; it is safe to leave unconnected.
    input  logic [XLEN-1:0]               fwd_query_addr_i,
    input  logic [2:0]                    fwd_query_size_i,
    output logic                          fwd_valid_o,
    output logic [XLEN-1:0]               fwd_data_o,
    output logic                          fwd_blocked_o,

    // -------------------------------------------------------------- status
    output logic [CNT_W-1:0]              o_count,
    output logic [CNT_W-1:0]              o_auth_cnt,
    output logic [ENTRIES-1:0]            o_occ,
    output logic [ENTRIES-1:0]            o_entry_authorised,
    output logic [31:0]                   o_alloc_ctr,
    output logic [31:0]                   o_fill_ctr,
    output logic [31:0]                   o_fill_stale_ctr,
    output logic [31:0]                   o_commit_ctr,
    output logic [31:0]                   o_commit_stale_ctr,
    output logic [31:0]                   o_drain_ctr,
    output logic [31:0]                   o_fault_ctr,
    output logic [31:0]                   o_squash_ctr,
    output logic [31:0]                   o_squash_spared_ctr,
    output logic [XLEN-1:0]               o_last_fault_tval,
    output logic [XLEN-1:0]               o_last_fault_cause,
    output mosaic_uop_pkg::uop_id_t       o_last_fault_id,

    // -------------------------------------------------------- observation
    // Every position, oldest first, zero beyond `o_count`, so a testbench can
    // compare the whole queue every cycle. A functional consumer has no business
    // reading it: the drain port is the interface.
    output logic [ENTRIES*ENTRY_W-1:0]    o_entry_pay
);

  // ------------------------------------------------------------ elaboration
  // The case is only meaningful with at least two entries (a store that bypasses
  // another needs a queue behind it) and at least two ROB index bits (a window
  // needs a wrap point). An elaboration error rather than a comment, so a
  // profile that made the case vacuous cannot ship as a silent pass.
  localparam bit TOO_FEW_ENTRIES = (ENTRIES < 2);
  localparam bit TOO_FEW_INDEX_BITS = (ROB_INDEX_W < 2);

  generate
    if (TOO_FEW_ENTRIES) begin : g_bad_entries
      mosaic_store_queue_contract_violation u_entries();
    end
    if (TOO_FEW_INDEX_BITS) begin : g_bad_index_bits
      mosaic_store_queue_contract_violation u_index_bits();
    end
  endgenerate

  // ------------------------------------------------------------- mutant knobs
  // One bit each so the shipping expression is one expression rather than four
  // copies of it. All are 1 in the shipping build.
`ifdef MOSAIC_SQ_MUTANT_VISIBLE_BEFORE_COMMIT
  localparam bit DRAIN_NEEDS_AUTH     = 1'b0;
`else
  localparam bit DRAIN_NEEDS_AUTH     = 1'b1;
`endif
`ifdef MOSAIC_SQ_MUTANT_EARLY_VISIBILITY
  localparam bit DRAIN_NEEDS_MATERIAL = 1'b0;
`else
  localparam bit DRAIN_NEEDS_MATERIAL = 1'b1;
`endif
`ifdef MOSAIC_SQ_MUTANT_FULL_FLUSH_DROPS_AUTHORISED
  localparam bit SQUASH_SPARES_AUTHORISED = 1'b0;
`else
  localparam bit SQUASH_SPARES_AUTHORISED = 1'b1;
`endif
`ifdef MOSAIC_SQ_MUTANT_ALLOC_DROP_WHEN_FULL
  localparam bit ALLOC_REQUIRES_ROOM  = 1'b0;
`else
  localparam bit ALLOC_REQUIRES_ROOM  = 1'b1;
`endif
`ifdef MOSAIC_SQ_MUTANT_DRAIN_DUPLICATE
  localparam bit DRAIN_REMOVES_ENTRY  = 1'b0;
`else
  localparam bit DRAIN_REMOVES_ENTRY  = 1'b1;
`endif
`ifdef MOSAIC_SQ_MUTANT_COMMIT_OUT_OF_ORDER
  localparam bit COMMIT_NAMES_NEXT    = 1'b0;
`else
  localparam bit COMMIT_NAMES_NEXT    = 1'b1;
`endif

  // ----------------------------------------------------------------- entry
  // What one speculative store is. `addr_valid` and `data_valid` are separate
  // because they arrive from different producers at different times, and
  // authorisation is *not* stored here: it is the watermark below.
  typedef struct packed {
    mosaic_uop_pkg::uop_id_t id;
    logic [XLEN-1:0]         base;
    logic [XLEN-1:0]         imm;
    logic [2:0]              size;
    logic [XLEN-1:0]         data;
    logic                    addr_valid;
    logic                    data_valid;
  } sq_entry_t;

  // ------------------------------------------------------------------ state
  // Entry storage is data and is deliberately not reset: validity lives in
  // `count_q`, there is no per-entry valid bit to keep in step with it, and a
  // position at or above `count_q` is masked to zero on the observation port --
  // the rule rtl/common/mosaic_ram.sv documents.
  sq_entry_t      ent_q [0:ENTRIES-1];
  logic [CNT_W-1:0] count_q;      // entries held
  logic [CNT_W-1:0] auth_cnt_q;   // entries from the head that are authorised

  // ------------------------------------------------------------- counters
  logic [31:0] alloc_ctr_q, fill_ctr_q, fill_stale_ctr_q;
  logic [31:0] commit_ctr_q, commit_stale_ctr_q;
  logic [31:0] drain_ctr_q, fault_ctr_q;
  logic [31:0] squash_ctr_q, squash_spared_ctr_q;
  logic [XLEN-1:0] last_fault_tval_q, last_fault_cause_q;
  mosaic_uop_pkg::uop_id_t last_fault_id_q;

  // ------------------------------------------------------------ resident set
  logic [ENTRIES-1:0] resident_c;
  logic [ENTRIES-1:0] auth_c;
  always_comb begin
    for (int unsigned i = 0; i < ENTRIES; i++) begin
      resident_c[i] = (CNT_W'(i) < count_q);
      auth_c[i]     = (CNT_W'(i) < auth_cnt_q);
    end
  end

  // -------------------------------------------------------------- the squash
  // Window arithmetic in the ROB's own modulus: both distances are computed
  // modulo 2**ROB_INDEX_W, so a window crossing the wrap point is exact.
  logic [ROB_INDEX_W-1:0] squash_win_len_c;
  assign squash_win_len_c = squash_tail_index_i - squash_from_index_i;

  logic [ENTRIES-1:0] squash_region_c;   // in the dead region
  logic [ENTRIES-1:0] squash_spared_c;   // ... and spared because authorised
  logic [ENTRIES-1:0] squash_drop_c;     // removed by the squash
  always_comb begin
    for (int unsigned i = 0; i < ENTRIES; i++) begin
      squash_region_c[i] = resident_c[i] && squash_valid_i &&
                           (squash_all_i ||
                            ((ent_q[i].id.rob_gen == squash_gen_i) &&
                             ((ent_q[i].id.rob_index - squash_from_index_i) <
                              squash_win_len_c)));
      squash_spared_c[i] = SQUASH_SPARES_AUTHORISED && squash_region_c[i] && auth_c[i];
      squash_drop_c[i]   = squash_region_c[i] && !squash_spared_c[i];
    end
  end

  logic [CNT_W-1:0] squash_drop_cnt_c;
  logic [CNT_W-1:0] squash_spared_cnt_c;
  always_comb begin
    squash_drop_cnt_c   = {CNT_W{1'b0}};
    squash_spared_cnt_c = {CNT_W{1'b0}};
    for (int unsigned i = 0; i < ENTRIES; i++) begin
      if (squash_drop_c[i])   squash_drop_cnt_c   = squash_drop_cnt_c   + CNT_W'(1);
      if (squash_spared_c[i]) squash_spared_cnt_c = squash_spared_cnt_c + CNT_W'(1);
    end
  end

  // ------------------------------------------------------- allocation accept
  logic alloc_room_c;
  assign alloc_room_c = (count_q < CNT_W'(ENTRIES)) || !ALLOC_REQUIRES_ROOM;
  assign alloc_ready_o = alloc_room_c;
  logic alloc_ok_c;
  assign alloc_ok_c = alloc_valid_i && alloc_room_c;

  sq_entry_t alloc_entry_c;
  always_comb begin
    alloc_entry_c.id         = alloc_id_i;
    alloc_entry_c.base       = alloc_base_i;
    alloc_entry_c.imm        = alloc_imm_i;
    alloc_entry_c.size       = alloc_size_i;
    alloc_entry_c.data       = alloc_data_i;
    alloc_entry_c.addr_valid = alloc_addr_valid_i;
    alloc_entry_c.data_valid = alloc_data_valid_i;
  end

  // ------------------------------------------------------------------ the fill
  // Matched by identity against the oldest resident entry. A fill that names no
  // resident entry is stale: the store either drained or was squashed.
  logic             fill_found_c;
  logic [IDX_W-1:0] fill_idx_c;
  always_comb begin
    fill_found_c = 1'b0;
    fill_idx_c   = {IDX_W{1'b0}};
    for (int unsigned i = 0; i < ENTRIES; i++) begin
      if (!fill_found_c && resident_c[i] && mosaic_uop_pkg::uop_id_eq(ent_q[i].id, fill_id_i)) begin
        fill_found_c = 1'b1;
        fill_idx_c   = IDX_W'(i);
      end
    end
  end

  assign fill_hit_o   = fill_valid_i && fill_found_c;
  assign fill_stale_o = fill_valid_i && !fill_found_c;
  logic fill_apply_c;
  assign fill_apply_c = fill_valid_i && fill_found_c;

  // The filled view of the current entries: the fill is applied to the entry it
  // names on the same edge it is accepted on, so a fill and a drain in one cycle
  // cannot disagree about what was captured.
  sq_entry_t ent_filled_c [0:ENTRIES-1];
  always_comb begin
    for (int unsigned i = 0; i < ENTRIES; i++) begin
      ent_filled_c[i] = ent_q[i];
      if (fill_apply_c && (fill_idx_c == IDX_W'(i))) begin
        if (fill_addr_valid_i) begin
          ent_filled_c[i].base = fill_base_i;
          ent_filled_c[i].imm  = fill_imm_i;
        end
        if (fill_data_valid_i) begin
          ent_filled_c[i].data = fill_data_i;
        end
        ent_filled_c[i].addr_valid = ent_q[i].addr_valid || fill_addr_valid_i;
        ent_filled_c[i].data_valid = ent_q[i].data_valid || fill_data_valid_i;
      end
    end
  end

  // -------------------------------------------------------- commit authority
  logic             commit_found_c;
  logic [IDX_W-1:0] commit_idx_c;
  always_comb begin
    commit_found_c = 1'b0;
    commit_idx_c   = {IDX_W{1'b0}};
    for (int unsigned i = 0; i < ENTRIES; i++) begin
      if (!commit_found_c && resident_c[i] &&
          mosaic_uop_pkg::uop_id_eq(ent_q[i].id, commit_id_i)) begin
        commit_found_c = 1'b1;
        commit_idx_c   = IDX_W'(i);
      end
    end
  end

  // The second authorisation of the cycle, matched by its own identity. A
  // `commit2_id_i` that names the entry `commit_valid_i` already names finds
  // the same index, and the "next unauthorised" test below then refuses it, so
  // one entry can never be authorised twice in one cycle.
  logic             commit2_found_c;
  logic [IDX_W-1:0] commit2_idx_c;
  always_comb begin
    commit2_found_c = 1'b0;
    commit2_idx_c   = {IDX_W{1'b0}};
    for (int unsigned i = 0; i < ENTRIES; i++) begin
      if (!commit2_found_c && resident_c[i] &&
          mosaic_uop_pkg::uop_id_eq(ent_q[i].id, commit2_id_i)) begin
        commit2_found_c = 1'b1;
        commit2_idx_c   = IDX_W'(i);
      end
    end
  end

  // The authorisation is applied only when it names the first unauthorised
  // entry. Comparing in the count domain matters at the boundary: with every
  // entry authorised, `auth_cnt_q == ENTRIES` and no resident index can equal
  // it, so a re-authorisation is refused rather than wrapped onto entry 0.
  logic commit_ok_c;
  assign commit_ok_c = commit_valid_i && commit_found_c &&
                       (COMMIT_NAMES_NEXT
                          ? (CNT_W'(commit_idx_c) == auth_cnt_q)
                          : 1'b1);
  assign commit_ok_o    = commit_ok_c;
  assign commit_stale_o = commit_valid_i && !commit_ok_c;

  // The second authorisation must name the entry immediately after the one the
  // first authorises this cycle: `auth_cnt_q` when the first was refused (or
  // absent), `auth_cnt_q + 1` when it was accepted. In the count domain, so it
  // cannot wrap onto a younger entry that happens to sit at index 0.
  logic commit2_ok_c;
  assign commit2_ok_c = commit2_valid_i && commit2_found_c &&
                        (COMMIT_NAMES_NEXT
                           ? (CNT_W'(commit2_idx_c) ==
                              (auth_cnt_q + CNT_W'(commit_ok_c)))
                           : 1'b1);
  assign commit2_ok_o    = commit2_ok_c;
  assign commit2_stale_o = commit2_valid_i && !commit2_ok_c;

  // The watermark after this cycle. It is *derived from the survivors* rather
  // than adjusted arithmetically: an entry counts only if it is still in the
  // queue and is authorised, and the watermark is the number of leading such
  // entries. That is the same statement as the per-entry flags' prefix
  // invariant, so a drain (which removes the authorised head), a squash (which
  // removes a suffix of the unauthorised region) and a commit in the same cycle
  // as either cannot leave the watermark counting an entry that is gone. The
  // ROB never retires in a flush cycle, so the last combination is not expected
  // in service -- but the invariant holds if it happens, which is what makes the
  // exported watermark comparable to the entries rather than merely plausible.
  logic [ENTRIES-1:0] auth_next_c;
  always_comb begin
    for (int unsigned i = 0; i < ENTRIES; i++) begin
      auth_next_c[i] = auth_c[i] || (commit_ok_c && (IDX_W'(i) == commit_idx_c))
                                 || (commit2_ok_c && (IDX_W'(i) == commit2_idx_c));
    end
  end

  // ---------------------------------------------------------------- the drain
  // Position 0 is the offer in the shipping build. The index is a register-like
  // combinational value rather than a constant so that the "any ready entry may
  // leave" defect is one selector change and not a second datapath.
  logic             drain_found_c;
  logic [IDX_W-1:0] drain_idx_c;
`ifdef MOSAIC_SQ_MUTANT_DRAIN_OUT_OF_ORDER
  // Mutant: the drain takes the youngest ready authorised entry it can find,
  // bypassing an older one that is not ready or not authorised.
  always_comb begin
    drain_found_c = 1'b0;
    drain_idx_c   = {IDX_W{1'b0}};
    for (int unsigned i = 0; i < ENTRIES; i++) begin
      if (resident_c[i] && auth_c[i] && ent_q[i].addr_valid && ent_q[i].data_valid) begin
        drain_found_c = 1'b1;
        drain_idx_c   = IDX_W'(i);
      end
    end
  end
`else
  assign drain_found_c = (count_q != {CNT_W{1'b0}});
  assign drain_idx_c   = {IDX_W{1'b0}};
`endif

`ifdef MOSAIC_SQ_MUTANT_DRAIN_OUT_OF_ORDER
  // The selector above already encodes "resident, authorised and ready", so the
  // shipping validity terms are not applied a second time.
  assign drain_req_valid_o = drain_found_c && !squash_drop_c[drain_idx_c];
`else
  assign drain_req_valid_o = drain_found_c &&
                             (DRAIN_NEEDS_AUTH
                                ? (auth_cnt_q != {CNT_W{1'b0}})
                                : 1'b1) &&
                             (DRAIN_NEEDS_MATERIAL
                                ? (ent_q[drain_idx_c].addr_valid &&
                                   ent_q[drain_idx_c].data_valid)
                                : 1'b1) &&
                             !squash_drop_c[drain_idx_c];
`endif

  // The request the memory side sees. Don't-care fields are don't-care when
  // `drain_req_valid_o` is low, which is the same rule mosaic_lsu_endpoint
  // states for its own payloads. The address is presented as base+imm, not as a
  // sum: the endpoint owns the one adder that turns it into an address, and a
  // second adder here is how the fault tval and the strobes start to disagree.
  always_comb begin
    drain_req_o.id         = ent_q[drain_idx_c].id;
    drain_req_o.we         = 1'b1;
    drain_req_o.base       = ent_q[drain_idx_c].base;
    drain_req_o.imm        = ent_q[drain_idx_c].imm;
    drain_req_o.size       = ent_q[drain_idx_c].size;
    drain_req_o.signed_    = 1'b0;
    drain_req_o.store_data = ent_q[drain_idx_c].data;
    // A store drain is never an atomic read-modify-write (I-039). The fields
    // exist in the packet and must be driven, not left unassigned: an
    // unassigned field is a value the endpoint would branch on.
    drain_req_o.is_amo     = 1'b0;
    drain_req_o.amo_op     = mosaic_pkg::AMO_ADD;
    drain_req_o.aq         = 1'b0;
    drain_req_o.rl         = 1'b0;
  end

  logic drain_accept_c;
  assign drain_accept_c = drain_req_valid_o && drain_req_ready_i;

  // Removing the accepted entry on the acceptance edge is the exactly-once rule.
  logic drain_removes_c;
  assign drain_removes_c = DRAIN_REMOVES_ENTRY && drain_accept_c;

  // The response is consumed unconditionally: the entry is gone, and the only
  // thing left to do with the answer is tally a fault. A faulting store wrote
  // nothing -- mosaic_lsu_endpoint traps it before the memory sees it.
  assign drain_rsp_ready_o = 1'b1;
  logic drain_rsp_c;
  assign drain_rsp_c = drain_rsp_valid_i && drain_rsp_ready_o;

  // ------------------------------------------------------------- next state
  sq_entry_t        ent_next_c [0:ENTRIES-1];
  logic [ENTRIES-1:0] keep_c;
  logic [CNT_W-1:0] next_count_c;

  logic             auth_open_c;
  logic [CNT_W-1:0] next_auth_cnt_c;

  always_comb begin
    // Default: keep the entry where it is. Every position is written on every
    // path, so the array cannot infer storage; positions at or above the new
    // occupancy are don't-care and are simply carried along.
    for (int unsigned i = 0; i < ENTRIES; i++) begin
      ent_next_c[i] = ent_q[i];
    end
    next_count_c    = {CNT_W{1'b0}};
    next_auth_cnt_c = {CNT_W{1'b0}};
    auth_open_c     = 1'b1;
    for (int unsigned i = 0; i < ENTRIES; i++) begin
      keep_c[i] = resident_c[i] && !squash_drop_c[i] &&
                  !(drain_removes_c && (IDX_W'(i) == drain_idx_c));
      if (keep_c[i]) begin
        ent_next_c[next_count_c[IDX_W-1:0]] = ent_filled_c[i];
        next_count_c = next_count_c + CNT_W'(1);
        // The watermark counts the *leading* survivors that are authorised, and
        // stops at the first one that is not: the authorised entries are always
        // a prefix, so there is nothing to find after that point.
        if (auth_open_c) begin
          if (auth_next_c[i]) next_auth_cnt_c = next_auth_cnt_c + CNT_W'(1);
          else                auth_open_c     = 1'b0;
        end
      end
    end
    // An accepted allocation is appended at the current occupancy. With
    // ALLOC_REQUIRES_ROOM the append cannot exceed ENTRIES; the mutant asserts
    // acceptance without room, and the guard below is what turns that into a
    // dropped entry (which the conservation identity then names) rather than an
    // out-of-range write. It is appended *after* the watermark is counted, and a
    // store cannot be authorised in its allocation cycle, so it never affects it.
    if (alloc_ok_c && (next_count_c < CNT_W'(ENTRIES))) begin
      ent_next_c[next_count_c[IDX_W-1:0]] = alloc_entry_c;
      next_count_c = next_count_c + CNT_W'(1);
    end
  end

  // ------------------------------------------------------------- outputs
  assign o_count    = count_q;
  assign o_auth_cnt = auth_cnt_q;

  always_comb begin
    for (int unsigned i = 0; i < ENTRIES; i++) begin
      o_occ[i]             = resident_c[i];
      o_entry_authorised[i] = resident_c[i] && auth_c[i];
    end
  end

  assign o_alloc_ctr         = alloc_ctr_q;
  assign o_fill_ctr          = fill_ctr_q;
  assign o_fill_stale_ctr    = fill_stale_ctr_q;
  assign o_commit_ctr        = commit_ctr_q;
  assign o_commit_stale_ctr  = commit_stale_ctr_q;
  assign o_drain_ctr         = drain_ctr_q;
  assign o_fault_ctr         = fault_ctr_q;
  assign o_squash_ctr        = squash_ctr_q;
  assign o_squash_spared_ctr = squash_spared_ctr_q;
  assign o_last_fault_tval   = last_fault_tval_q;
  assign o_last_fault_cause  = last_fault_cause_q;
  assign o_last_fault_id     = last_fault_id_q;

  always_comb begin
    o_entry_pay = {ENTRIES*ENTRY_W{1'b0}};
    for (int unsigned i = 0; i < ENTRIES; i++) begin
      if (resident_c[i]) begin
        o_entry_pay[i*ENTRY_W + OFF_DATA_VALID +: 1]      = ent_q[i].data_valid;
        o_entry_pay[i*ENTRY_W + OFF_ADDR_VALID +: 1]      = ent_q[i].addr_valid;
        o_entry_pay[i*ENTRY_W + OFF_DATA +: XLEN]         = ent_q[i].data;
        o_entry_pay[i*ENTRY_W + OFF_SIZE +: 3]            = ent_q[i].size;
        o_entry_pay[i*ENTRY_W + OFF_IMM +: XLEN]          = ent_q[i].imm;
        o_entry_pay[i*ENTRY_W + OFF_BASE +: XLEN]         = ent_q[i].base;
        o_entry_pay[i*ENTRY_W + OFF_ID +: ID_W]           = ent_q[i].id;
      end
    end
  end

  // ------------------------------------------------------------ forwarding
  // The rule is in the header. `covers` is the containment test for the queried
  // byte range inside the doubleword both addresses select; the lane arithmetic
  // is done in four bits because an access can never own more than eight bytes.
  logic [3:0] fwd_q_bytes_c;
  assign fwd_q_bytes_c = 4'(mosaic_uop_pkg::size_bytes(fwd_query_size_i));

  function automatic logic FwdCovers(input logic [XLEN-1:0] e_addr, input logic [2:0] e_size);
    logic [3:0] e_lo, q_lo, e_n;
    begin
      e_lo = 4'(e_addr[2:0]);
      q_lo = 4'(fwd_query_addr_i[2:0]);
      e_n  = 4'(mosaic_uop_pkg::size_bytes(e_size));
      FwdCovers = (e_addr[XLEN-1:3] == fwd_query_addr_i[XLEN-1:3]) &&
                  (e_lo <= q_lo) && ((q_lo + fwd_q_bytes_c) <= (e_lo + e_n));
    end
  endfunction

  logic             fwd_found_c;
  logic [IDX_W-1:0] fwd_idx_c;
  logic             fwd_blocked_c;
  always_comb begin
    fwd_found_c   = 1'b0;
    fwd_idx_c     = {IDX_W{1'b0}};
    fwd_blocked_c = 1'b0;
    for (int unsigned i = 0; i < ENTRIES; i++) begin
      // I-038: an entry whose address is a device (non-idempotent) region is
      // invisible to this query. It is not a source -- a device access must
      // never be merged into an ordinary load -- and it is not a blocker
      // either, because its bytes are in a region no ordinary load can alias,
      // so a load waiting for it would wait for nothing. `is_device_addr` is
      // the same predicate the serializer uses, so the query and the gate
      // cannot disagree about which addresses are devices.
      if (resident_c[i] && mosaic_uop_pkg::is_device_addr(ent_q[i].base + ent_q[i].imm)) begin
        continue;
      end
      if (resident_c[i] && !ent_q[i].addr_valid) begin
        fwd_blocked_c = 1'b1;
      end
      // The last match in program order is the youngest one, which is the one
      // the load must use.
      if (resident_c[i] && ent_q[i].addr_valid && ent_q[i].data_valid &&
          FwdCovers(ent_q[i].base + ent_q[i].imm, ent_q[i].size)) begin
        fwd_found_c = 1'b1;
        fwd_idx_c   = IDX_W'(i);
      end
    end
  end

  // The lane the matched store's data sits at. Only the low three bits of
  // `base + imm` are needed to place the payload, and the low three bits of the
  // sum are the sum of the low three bits, so no wider adder is inferred.
  logic [2:0] fwd_match_lane_c;
  assign fwd_match_lane_c = ent_q[fwd_idx_c].base[2:0] + ent_q[fwd_idx_c].imm[2:0];
  assign fwd_valid_o   = fwd_found_c;
  assign fwd_data_o    = ent_q[fwd_idx_c].data << {fwd_match_lane_c, 3'b000};
  assign fwd_blocked_o = fwd_blocked_c;

  // -------------------------------------------------------------- the edge
  always_ff @(posedge clk) begin
    if (rst) begin
      count_q             <= {CNT_W{1'b0}};
      auth_cnt_q          <= {CNT_W{1'b0}};
      alloc_ctr_q         <= 32'd0;
      fill_ctr_q          <= 32'd0;
      fill_stale_ctr_q    <= 32'd0;
      commit_ctr_q        <= 32'd0;
      commit_stale_ctr_q  <= 32'd0;
      drain_ctr_q         <= 32'd0;
      fault_ctr_q         <= 32'd0;
      squash_ctr_q        <= 32'd0;
      squash_spared_ctr_q <= 32'd0;
      last_fault_tval_q   <= {XLEN{1'b0}};
      last_fault_cause_q  <= {XLEN{1'b0}};
      last_fault_id_q     <= {ID_W{1'b0}};
    end else begin
      for (int unsigned i = 0; i < ENTRIES; i++) begin
        ent_q[i] <= ent_next_c[i];
      end
      count_q    <= next_count_c;
      auth_cnt_q <= next_auth_cnt_c;

      if (alloc_ok_c)         alloc_ctr_q        <= alloc_ctr_q + 32'd1;
      if (fill_apply_c)       fill_ctr_q         <= fill_ctr_q + 32'd1;
      if (fill_stale_o)       fill_stale_ctr_q   <= fill_stale_ctr_q + 32'd1;
      // Both ports can authorise in one cycle, so the increment is the sum. Two
      // separate non-blocking assignments to one signal would make the second
      // overwrite the first, and the counter would silently under-count exactly
      // the back-to-back stores the second port exists for.
      commit_ctr_q       <= commit_ctr_q + {31'd0, commit_ok_c} + {31'd0, commit2_ok_c};
      commit_stale_ctr_q <= commit_stale_ctr_q + {31'd0, commit_stale_o} +
                            {31'd0, commit2_stale_o};
      if (drain_accept_c)     drain_ctr_q        <= drain_ctr_q + 32'd1;
      if (squash_drop_cnt_c != {CNT_W{1'b0}}) begin
        squash_ctr_q <= squash_ctr_q + 32'(squash_drop_cnt_c);
      end
      if (squash_spared_cnt_c != {CNT_W{1'b0}}) begin
        squash_spared_ctr_q <= squash_spared_ctr_q + 32'(squash_spared_cnt_c);
      end

      if (drain_rsp_c && drain_rsp_i.fault) begin
        fault_ctr_q       <= fault_ctr_q + 32'd1;
        last_fault_tval_q <= drain_rsp_i.tval;
        last_fault_cause_q <= drain_rsp_i.cause;
        last_fault_id_q   <= drain_rsp_i.id;
      end
    end
  end

endmodule : mosaic_store_queue

`resetall
`default_nettype wire

`endif  // MOSAIC_STORE_QUEUE_SV_
