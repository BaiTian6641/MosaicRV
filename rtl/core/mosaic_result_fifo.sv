// ============================================================================
// mosaic_result_fifo -- work package I-025: one cluster's completion buffer.
//
// The execution units of a cluster finish on schedules that have nothing to do
// with each other: a fixed-latency ALU, a variable-latency MUL/DIV and an LSU
// whose load returns when memory says so. Several of them can finish in the same
// cycle, and there are deliberately fewer result slots than units. This module is
// where those completions are held until the consumer (I-026's writeback
// arbiter) takes them.
//
// Everything here is sized from the generated configuration package
// (`mosaic_cfg_pkg`) and the generated identity package (`mosaic_id_pkg`). There
// is no depth, no port count and no field width written as a literal, and the
// parameters are `localparam` rather than `parameter` so a caller cannot supply
// a second geometry. MOSAIC_CLUSTERS is deliberately *not* referenced: this
// module is one cluster's buffer and knows nothing about how many clusters
// exist.
//
// ------------------------------------------------- why one port per unit
//
// The card's Fail criterion is "assuming multiply and load never return at the
// same time". That assumption is only *testable* if the interface can represent
// both returning at once, so the interface has one producer port per possible
// completing source, and the port count is derived rather than chosen:
//
//     PRODUCERS = MOSAIC_ALU_PER_CLUSTER + MOSAIC_MULDIV_UNITS + MOSAIC_LSU_UNITS
//
// At p0 that is 1 + 1 + 1 = 3 producers into MOSAIC_RESULT_FIFO = 2 slots, which
// is exactly the situation the card describes: a collision that has to lose
// nothing. A design that folded the sources onto fewer ports would make the
// failure mode unrepresentable -- and therefore untestable -- rather than
// handled, which is the defect the card names. The two pool units (the shared
// iterative MUL/DIV and the LSU) complete into the cluster that owns the uop;
// this module models the cluster end of that routing, not the routing.
//
// --------------------------------------- what "loses nothing" means, exactly
//
// The FIFO does **not** buffer a losing producer's result, and that is not a
// loss. The handshake is the standard valid/ready one and it is the *producer's*
// obligation to hold `*_valid` and its payload until `*_ready` is high; the FIFO
// simply refuses the ones it cannot take this cycle (`*_ready` low), and the
// producer keeps offering the same result. Nothing is sampled, captured or
// dropped on a refusal. A result is therefore lost only if the design tells a
// producer it was accepted without storing it -- which is what several of the
// negative controls below do, and what the unit test is built to detect.
//
// Arbitration among producers that can be accepted is **fixed priority by
// producer index**: producer 0 first, then 1, and so on, up to the number of
// currently free slots. Fixed priority (rather than round-robin) is chosen
// deliberately: the ordering of same-cycle completions has to be reproducible
// for the queue's contents to be a defined function of the input history, and
// a rotating priority would make the accepted order depend on history in a way
// nothing in the pipeline needs. It cannot starve a producer: a refused producer
// is refused only while the queue is full, and the queue empties at the
// consumer's rate, at which point the refusal ends. There is no state that a
// lower-indexed producer can hold, so it cannot block a higher one beyond the
// cycle in which it is itself accepted.
//
// ----------------------------------------------------- one payload word, and
// --------------------------------------------- why the exception cannot be lost
//
// A result is a single packed word, not a data bus plus a side-channel, because
// the exception has to travel *with* the result rather than be looked up next to
// it. The failure this package exists to prevent is a queue that delivers an
// ordinary result into a slot whose exception payload it has just overwritten;
// there is no such slot here. Each entry owns its own exception bit, cause and
// tval, and a push writes all three (or clears all three) atomically:
//
//     | field      | width             | source                                  |
//     |------------|-------------------|-----------------------------------------|
//     | rob_index  | MOSAIC_ID_W_ROB_INDEX | identity of the macro that produced it |
//     | rob_gen    | MOSAIC_ID_W_ROB_GEN   | generation, so a recycled slot is safe |
//     | uop_index  | MOSAIC_ID_W_UOP_INDEX | which child within the macro           |
//     | data       | MOSAIC_XLEN       | the value to write back                  |
//     | exc_valid  | 1                 | this completion raised an exception      |
//     | exc_cause  | MOSAIC_XLEN       | the cause code (mosaic_pkg::EXC_*), 64b  |
//     | exc_tval   | MOSAIC_XLEN       | the trap value                           |
//
// The layout is most-significant-field-first in exactly that order, so the
// identity occupies the top bits, mirroring mosaic_iq's `{rob_index, rob_gen,
// uop_index}` word. `data` is present even for an exceptional result: it is
// architecturally meaningless there, and carrying it unchanged is cheaper and
// less surprising than a mux that would have to know which exceptions are
// "data-bearing". Cause and tval are XLEN wide because that is the width
// mosaic_retire's payload ports (`pay_exc_cause`, `pay_exc_tval`) already use;
// narrowing here would create a second, incompatible exception format.
//
// The identity carries rob_gen, not just rob_index, for the same reason
// mosaic_rob and mosaic_rename do: the index wraps, and a completion that
// outlives its generation names a *different* macro. This module does not
// compare the generation -- it has no reference to compare against, and the ROB
// is the structure that owns that decision -- but it must not destroy the field
// that makes the comparison possible. The consumer receives the full identity.
//
// --------------------------------------------------------------------- kill
//
// `kill_valid` discards speculative work that recovery has decided is dead, and
// the predicate is precisely:
//
//     kill hits an entry  <=>  kill_valid && (kill_all
//                              || (entry.rob_index == kill_rob_index
//                                  && entry.rob_gen   == kill_rob_gen))
//
// `uop_index` is deliberately **not** compared: a kill names a *macro*, and
// every child of that macro must go. This is the same rule and the same reason
// as mosaic_iq's kill port (`UopIsMacro`), and it is why the kill side carries
// two fields while the identity carries three.
//
// A kill does two things in the cycle it is asserted:
//
//   1. Every buffered entry that matches is dropped before it can be presented.
//      The consumer port masks the head combinationally (`c_valid` is low for a
//      killed head), so a killed entry is never delivered even if the consumer
//      happened to be ready that cycle -- a pop and a kill can therefore never
//      both apply to the same entry. Each buffered drop increments `o_kill_ctr`.
//
//   2. A producer offering a result that matches the kill predicate is **absorbed
//      and dropped**: `*_ready` is asserted, the result is not stored, and it is
//      counted in `o_kill_offer_ctr`. It is *not* refused. Refusing it is the
//      alternative, and it is a livelock: the producer would hold a result for an
//      instruction that no longer exists, no event would ever make the queue
//      accept it, and the producer has no other way to learn the uop is dead.
//      mosaic_iq documents the same exception for its grant port ("the one
//      documented exception to 'hold until accepted'"), and this module follows
//      it. The handshake completes normally, so the producer withdraws; the
//      result is accounted for as killed and never reaches the queue.
//
// `o_kill_total = o_kill_ctr + o_kill_offer_ctr` is the full count of results the
// kill accounted for, whether they were already buffered or still in flight.
//
// ----------------------------------------------------------- storage and shape
//
// The queue is an **ordered compacted sequence**, not a circular pointer pair.
// Position 0 is the oldest entry and the consumer sees position 0; positions
// `0 .. o_count-1` hold entries and the rest are zero. On every edge the queue is
// rebuilt from three sources, in this order:
//
//     survivors (buffered entries, oldest first, minus pops and kills)
//       ++ granted pushes (ascending producer index)
//
// so a kill compacts the survivors towards position 0 and a same-cycle pop and
// push cannot alias: there is no pair of pointers that can be confused with each
// other, and the "same-cycle pop frees the head slot while a push fills the tail
// slot" aliasing bug of a naive pointer FIFO is unrepresentable. The cost is
// O(ENTRIES) write ports (each position is muxed from every source), which is the
// right trade at MOSAIC_RESULT_FIFO = 2 and is stated here rather than discovered
// later: a deep result FIFO would want a pointer-based circular buffer with the
// same interface and the same contract, and would keep the ordered observation
// ports by muxing them for the testbench instead of by shifting storage.
//
// Reset is synchronous and active high. It clears the control state -- occupancy,
// the pointers' moral equivalent (`n`), and the counters. It clears **nothing
// else**: the entry storage is data, and validity lives entirely in `n` (there is
// no per-entry valid bit to keep in step with it), which is the rule
// rtl/common/mosaic_ram.sv documents. Positions at or above `n` are masked to
// zero on the observation port, so a testbench can compare the whole vector every
// cycle without reading unreset storage.
//
// Input back-pressure: `*_ready` is a function of the registered occupancy and
// the kill inputs only. It does **not** consult the consumer's `c_ready`, so there
// is no combinational path from the writeback arbiter back into the execution
// units -- the same trade mosaic_fifo documents, and the reason a same-cycle pop
// does not free a slot for a same-cycle push. A refused push is offered again next
// cycle; nothing is lost by waiting one cycle. `c_valid` and `c_pay` are likewise
// functions of registers (plus the kill inputs), so the queue is not a
// combinational path from its producers to its consumer either.
//
// ---------------------------------------------------------- conservation
//
// The counters are exported so the property can be checked from outside rather
// than argued:
//
//     o_push_ctr == o_pop_ctr + o_kill_ctr + o_count
//
// Every accepted result leaves exactly once (pushed and still held, popped, or
// killed), and an absorbed in-flight offer is *not* a push (it was never stored),
// so it appears only in `o_kill_offer_ctr` and never disturbs the identity. A pop
// that was not pushed, a duplicate pop and a lost push are each detectable
// because each breaks the identity by exactly its own count. The counters are 32
// bits: the runner's maximum campaign is far below 2**32 cycles, so they cannot
// wrap inside a test, and a wrap outside one is a board-level event, not a
// microarchitectural one.
//
// ------------------------------------------------------------- negative controls
//
// -DMOSAIC_RESULT_FIFO_MUTANT_<n> injects one defect the case must detect. None
// is defined in the shipping build. The table with real counts is in
// results/reports/I-025-result-fifo.md:
//
//   DROP_ON_COLLISION  every offering producer is told its result was taken, but
//                      only the first is stored: the collision loser is lost.
//   VALID_PULSE        an offer counts only on the cycle `*_valid` *rises*, so a
//                      producer that has to hold its result is never accepted.
//   EXC_DROPPED        any push clears the buffered exception flags, so an
//                      ordinary result erases an outstanding exception.
//   OCC_OFF_BY_ONE     the exported occupancy is one less than the truth.
//   KILL_DELIVERS      a killed head is still presented to the consumer.
//   KILL_NOT_COUNTED   buffered kills are dropped but not counted.
// ============================================================================

`default_nettype none
`resetall

/* verilator lint_off UNUSEDPARAM */
/* verilator lint_off UNUSEDSIGNAL */
// The generated headers declare one localparam per knob for the whole project.
// This module names the execution-fabric subset; the rest belong to other modules
// and are unused *here* by construction, not by omission. The identity package
// also carries helper functions whose unused bits Verilator reports by name --
// they are the generated contract's own helpers, not this module's code, and the
// report is about bodies this file does not call.
`include "mosaic_cfg_pkg.svh"
// `mosaic_id_pkg.svh` carries no include guard, so this is the only place in this
// compilation unit that may include it. The testbench wrapper therefore does not
// include it either; it reads the identity widths back out of the elaborated
// instance instead. (Including it twice would be a duplicate package definition
// that Verilator tolerates and slang rejects, which is exactly how the missing
// guard on the config header was found.)
`include "mosaic_id_pkg.svh"
/* verilator lint_on UNUSEDSIGNAL */
/* verilator lint_on UNUSEDPARAM */

module mosaic_result_fifo #(
    // Geometry, from the generated packages only. `localparam`, not `parameter`:
    // a caller-supplied default would be a second geometry.
    localparam int unsigned ENTRIES     = mosaic_cfg_pkg::MOSAIC_RESULT_FIFO,
    localparam int unsigned XLEN        = mosaic_cfg_pkg::MOSAIC_XLEN,
    localparam int unsigned ROB_INDEX_W = mosaic_id_pkg::MOSAIC_ID_W_ROB_INDEX,
    localparam int unsigned ROB_GEN_W   = mosaic_id_pkg::MOSAIC_ID_W_ROB_GEN,
    localparam int unsigned UOP_W       = mosaic_id_pkg::MOSAIC_ID_W_UOP_INDEX,

    // One producer port per possible completing source in this cluster; see the
    // header for why the count is the sum and not a smaller, folded number.
    localparam int unsigned N_ALU     = mosaic_cfg_pkg::MOSAIC_ALU_PER_CLUSTER,
    localparam int unsigned N_MULDIV  = mosaic_cfg_pkg::MOSAIC_MULDIV_UNITS,
    localparam int unsigned N_LSU     = mosaic_cfg_pkg::MOSAIC_LSU_UNITS,
    localparam int unsigned PRODUCERS = N_ALU + N_MULDIV + N_LSU,

    // Derived, so not overridable. CNT_W can hold 0..ENTRIES: the occupancy is a
    // count, not an index, so it needs the extra bit.
    localparam int unsigned CNT_W = $clog2(ENTRIES + 1),

    // Payload field offsets, least significant bit first. See the layout table in
    // the header; the top field is the identity, matching mosaic_iq's word order.
    localparam int unsigned OFF_TVAL  = 0,
    localparam int unsigned OFF_CAUSE = OFF_TVAL + XLEN,
    localparam int unsigned OFF_EXC   = OFF_CAUSE + XLEN,
    localparam int unsigned OFF_DATA  = OFF_EXC + 1,
    localparam int unsigned OFF_UOP   = OFF_DATA + XLEN,
    localparam int unsigned OFF_GEN   = OFF_UOP + UOP_W,
    localparam int unsigned OFF_INDEX = OFF_GEN + ROB_GEN_W,
    localparam int unsigned ENTRY_W   = OFF_INDEX + ROB_INDEX_W
) (
    input  logic                          clk,
    input  logic                          rst,

    // --------------------------------------------------- completion producers
    // `p_pay` is PRODUCERS payload words laid back to back, producer 0 in the
    // least significant slice. `*_valid` must be held with a stable payload until
    // `*_ready` is high; the transfer is exactly `valid && ready`.
    input  logic [PRODUCERS-1:0]          p_valid,
    output logic [PRODUCERS-1:0]          p_ready,
    input  logic [PRODUCERS*ENTRY_W-1:0]  p_pay,

    // ------------------------------------------------------------- consumer
    output logic                          c_valid,
    input  logic                          c_ready,
    output logic [ENTRY_W-1:0]            c_pay,

    // ------------------------------------------------------------------ kill
    // Predicate: kill_valid && (kill_all || (rob_index,rob_gen) match). See the
    // header; uop_index is deliberately not part of it.
    input  logic                          kill_valid,
    input  logic                          kill_all,
    input  logic [ROB_INDEX_W-1:0]        kill_rob_index,
    input  logic [ROB_GEN_W-1:0]          kill_rob_gen,

    // ---------------------------------------------------------------- status
    output logic [CNT_W-1:0]              o_count,
    output logic [ENTRIES-1:0]            o_occ,        // one bit per position
    output logic [ENTRIES-1:0]            o_exc_occ,    // positions holding an exception
    output logic [31:0]                   o_push_ctr,   // results stored
    output logic [31:0]                   o_pop_ctr,    // results delivered
    output logic [31:0]                   o_kill_ctr,   // buffered entries dropped
    output logic [31:0]                   o_kill_offer_ctr,  // in-flight offers absorbed
    output logic [31:0]                   o_kill_total,      // the two above, summed

    // ----------------------------------------------------------- observation
    // Every position, oldest first, zero for positions at or above o_count, so a
    // testbench can compare the whole queue every cycle. A functional consumer
    // has no business reading it: `c_valid`/`c_pay` are the interface.
    output logic [ENTRIES*ENTRY_W-1:0]    o_entry_pay
);

  // ------------------------------------------------------------------ guards
  // The card is only meaningful if a collision is representable: at least two
  // producers into at least one slot. An elaboration error rather than a comment,
  // so a profile that made the case vacuous cannot ship as a silent pass.
  localparam bit NO_ENTRIES        = (ENTRIES < 1);
  localparam bit TOO_FEW_PRODUCERS = (PRODUCERS < 2);

  generate
    if (NO_ENTRIES) begin : g_bad_entries
      mosaic_result_fifo_contract_violation u_entries();
    end
    if (TOO_FEW_PRODUCERS) begin : g_bad_producers
      mosaic_result_fifo_contract_violation u_producers();
    end
  endgenerate

  // ---------------------------------------------------------------- helpers
  // Field extraction from a packed payload word. One place per field, so the
  // layout above has exactly one implementation. Each extractor reads one slice
  // of its argument and ignores the rest, which is the point of the function, so
  // the "unused bits of a function variable" report is silenced here rather than
  // worked around by comparing fields the function does not need.
  /* verilator lint_off UNUSEDSIGNAL */
  function automatic logic [ROB_INDEX_W-1:0] PayIndex(input logic [ENTRY_W-1:0] pay);
    PayIndex = pay[OFF_INDEX +: ROB_INDEX_W];
  endfunction

  function automatic logic [ROB_GEN_W-1:0] PayGen(input logic [ENTRY_W-1:0] pay);
    PayGen = pay[OFF_GEN +: ROB_GEN_W];
  endfunction

  function automatic logic PayExc(input logic [ENTRY_W-1:0] pay);
    PayExc = pay[OFF_EXC];
  endfunction
  /* verilator lint_on UNUSEDSIGNAL */

  // Clear the exception flag, leaving every other field alone. Used only by the
  // EXC_DROPPED negative control; the shipping build does not call it, so it is
  // wrapped in the same `ifdef as its caller to keep the linted design free of
  // dead logic.
`ifdef MOSAIC_RESULT_FIFO_MUTANT_EXC_DROPPED
  function automatic logic [ENTRY_W-1:0] ClearExc(input logic [ENTRY_W-1:0] pay);
    // A mask with every bit set except the exception flag, built by shifting a
    // one into position and inverting: no chained part-select, which two linters
    // read differently.
    ClearExc = pay & ~(ENTRY_W'(1) << OFF_EXC);
  endfunction
`endif

  // The kill predicate, in one place. Comparing rob_index and rob_gen but not
  // uop_index is the documented macro-level rule (see the header), not an
  // omission: the low bits of the identity are genuinely unused here, which is
  // what the function is for.
  /* verilator lint_off UNUSEDSIGNAL */
  function automatic logic KillHits(input logic [ENTRY_W-1:0] pay);
    KillHits = kill_valid &&
               (kill_all ||
                ((PayIndex(pay) == kill_rob_index) && (PayGen(pay) == kill_rob_gen)));
  endfunction
  /* verilator lint_on UNUSEDSIGNAL */

  // ------------------------------------------------------------------- state
  // Entry storage, oldest at position 0. Deliberately not reset: validity is
  // carried by `n` alone, and clearing ENTRIES payload words on every reset would
  // be paying reset cost for data.
  logic [ENTRY_W-1:0] ent [0:ENTRIES-1];

  logic [CNT_W-1:0]   n;              // occupancy: 0..ENTRIES
  logic [31:0]        push_ctr;
  logic [31:0]        pop_ctr;
  logic [31:0]        kill_ctr;
  logic [31:0]        kill_offer_ctr;

  // The occupancy widened to 32 bits once, so every `j < n` comparison below is
  // width-exact against the 32-bit loop index instead of relying on an implicit
  // extension.
  logic [31:0] n_wide;
  assign n_wide = 32'(n);

  // ------------------------------------------------------------- consumer side
  logic head_hit;     // the oldest entry is killed this cycle
  logic pop_now;      // a delivery actually happens this cycle

  assign head_hit = (n != {CNT_W{1'b0}}) && KillHits(ent[0]);

  // A killed head is masked combinationally, so it can never be delivered even if
  // the consumer is ready: the kill wins over the pop, and the two can therefore
  // never both apply to one entry (see the pop term in the rebuild below).
  assign c_valid = (n != {CNT_W{1'b0}}) && !head_hit && !rst;
  assign c_pay   = ent[0];
  assign pop_now = c_valid && c_ready;

  // Buffered entries the kill drops this cycle. Gated on `j < n` so the matcher
  // never reads a position that holds no entry.
  logic [ENTRIES-1:0] store_kill;

  always_comb begin
    for (int unsigned j = 0; j < ENTRIES; j++) begin
      store_kill[j] = (j < n_wide) && KillHits(ent[j]);
    end
  end

  // ------------------------------------------------------------- producer side
  logic [PRODUCERS-1:0] absorb;   // offering, killed: consumed and dropped
  logic [PRODUCERS-1:0] offer;    // offering, needs a slot
  logic [PRODUCERS-1:0] grant;    // offered and accepted this cycle
  logic [CNT_W-1:0]     free_slots;

  // Producer readiness is a function of the registered occupancy and the kill
  // inputs, never of `c_ready`: a same-cycle pop does not free a slot for a
  // same-cycle push. The refused push is simply offered again next cycle.
  assign free_slots = CNT_W'(ENTRIES) - n;

`ifdef MOSAIC_RESULT_FIFO_MUTANT_VALID_PULSE
  // NEGATIVE CONTROL: an offer counts only on the cycle `*_valid` rises, so a
  // producer that has to hold its result while the queue is full is never
  // accepted. This is the "valid pulse that cannot be held" assumption the card
  // names, expressed as a design that requires it.
  logic [PRODUCERS-1:0] p_valid_q;
  always_ff @(posedge clk) begin
    if (rst) p_valid_q <= {PRODUCERS{1'b0}};
    else     p_valid_q <= p_valid;
  end
`endif

  always_comb begin
    for (int unsigned p = 0; p < PRODUCERS; p++) begin
      absorb[p] = p_valid[p] && !rst && KillHits(p_pay[p*ENTRY_W +: ENTRY_W]);
`ifdef MOSAIC_RESULT_FIFO_MUTANT_VALID_PULSE
      offer[p]  = p_valid[p] && !p_valid_q[p] && !rst && !absorb[p];
`else
      offer[p]  = p_valid[p] && !rst && !absorb[p];
`endif
    end
  end

`ifdef MOSAIC_RESULT_FIFO_MUTANT_DROP_ON_COLLISION
  // NEGATIVE CONTROL: every offering producer is told its result was taken, but
  // only the first one is stored. The collision loser is lost, which is the
  // failure the package exists to prevent.
  always_comb begin
    grant[0] = offer[0] && (free_slots != {CNT_W{1'b0}});
    for (int unsigned p = 1; p < PRODUCERS; p++) grant[p] = 1'b0;
  end
  assign p_ready = absorb | offer;
`else
  // Fixed priority by producer index, up to the number of free slots. Written as
  // a running count rather than a per-producer comparison so it stays a prefix
  // rule for any PRODUCERS.
  always_comb begin
    logic [CNT_W-1:0] taken;
    taken = {CNT_W{1'b0}};
    for (int unsigned p = 0; p < PRODUCERS; p++) begin
      grant[p] = offer[p] && (taken < free_slots);
      if (grant[p]) taken = taken + CNT_W'(1);
    end
  end
  assign p_ready = absorb | grant;
`endif

  // ------------------------------------------------------- next queue contents
  // The post-edge ordered list: survivors (oldest first, minus the popped head and
  // the killed entries), then the granted pushes in ascending producer order.
  // Every destination is muxed from every source with constant indices, so no
  // variable-index array write reaches a synthesis tool.
  logic [ENTRY_W-1:0] nx_ent [0:ENTRIES-1];
  logic [CNT_W-1:0]   nx_n;

  always_comb begin
    logic [CNT_W-1:0]   w;       // running destination index
    logic [ENTRY_W-1:0] pushed;

    w = {CNT_W{1'b0}};
    pushed = {ENTRY_W{1'b0}};
    for (int unsigned d = 0; d < ENTRIES; d++) nx_ent[d] = {ENTRY_W{1'b0}};

    for (int unsigned j = 0; j < ENTRIES; j++) begin
      if ((j < n_wide) && !store_kill[j] && !((j == 0) && pop_now)) begin
        for (int unsigned d = 0; d < ENTRIES; d++) begin
          if (w == CNT_W'(d)) nx_ent[d] = ent[j];
        end
        w = w + CNT_W'(1);
      end
    end

    for (int unsigned p = 0; p < PRODUCERS; p++) begin
      if (grant[p]) begin
        pushed = p_pay[p*ENTRY_W +: ENTRY_W];
`ifdef MOSAIC_RESULT_FIFO_MUTANT_EXC_DROPPED
        // NEGATIVE CONTROL: an ordinary result that follows an exceptional one
        // erases the exception. Applied to the pushed word here and to every
        // buffered entry below, which is what "overwritten by an ordinary result"
        // looks like in a queue that stores the exception beside the data.
        pushed = ClearExc(pushed);
`endif
        for (int unsigned d = 0; d < ENTRIES; d++) begin
          if (w == CNT_W'(d)) nx_ent[d] = pushed;
        end
        w = w + CNT_W'(1);
      end
    end

`ifdef MOSAIC_RESULT_FIFO_MUTANT_EXC_DROPPED
    if (|grant) begin
      for (int unsigned d = 0; d < ENTRIES; d++) nx_ent[d] = ClearExc(nx_ent[d]);
    end
`endif

    nx_n = w;
  end

  // --------------------------------------------------------------- increments
  logic [31:0] push_add;
  logic [31:0] kill_add;
  logic [31:0] kill_offer_add;

  always_comb begin
    push_add       = 32'd0;
    kill_add       = 32'd0;
    kill_offer_add = 32'd0;
    for (int unsigned p = 0; p < PRODUCERS; p++) begin
      if (grant[p])  push_add       = push_add + 32'd1;
      if (absorb[p]) kill_offer_add = kill_offer_add + 32'd1;
    end
    for (int unsigned j = 0; j < ENTRIES; j++) begin
      if (store_kill[j]) kill_add = kill_add + 32'd1;
    end
  end

  // ------------------------------------------------------------------ registers
  // Synchronous, active-high reset. `ent` is not reset: see the header.
  always_ff @(posedge clk) begin
    if (rst) begin
      n              <= {CNT_W{1'b0}};
      push_ctr       <= 32'd0;
      pop_ctr        <= 32'd0;
      kill_ctr       <= 32'd0;
      kill_offer_ctr <= 32'd0;
    end else begin
      n <= nx_n;
      for (int unsigned d = 0; d < ENTRIES; d++) ent[d] <= nx_ent[d];
      push_ctr       <= push_ctr + push_add;
      kill_ctr       <= kill_ctr + kill_add;
      kill_offer_ctr <= kill_offer_ctr + kill_offer_add;
      if (pop_now) pop_ctr <= pop_ctr + 32'd1;
`ifdef MOSAIC_RESULT_FIFO_MUTANT_KILL_NOT_COUNTED
      // NEGATIVE CONTROL: buffered kills are dropped but not counted, so the
      // conservation identity breaks the moment recovery kills anything.
      kill_ctr <= kill_ctr;
`endif
    end
  end

  // ------------------------------------------------------------------- status
  // Occupancy is the count itself; the negative control below subtracts one, so
  // the exported number and the queue disagree by exactly one entry.
`ifdef MOSAIC_RESULT_FIFO_MUTANT_OCC_OFF_BY_ONE
  assign o_count = (n == {CNT_W{1'b0}}) ? {CNT_W{1'b0}} : (n - CNT_W'(1));
`else
  assign o_count = n;
`endif
  assign o_kill_total    = kill_ctr + kill_offer_ctr;
  assign o_push_ctr      = push_ctr;
  assign o_pop_ctr       = pop_ctr;
  assign o_kill_ctr      = kill_ctr;
  assign o_kill_offer_ctr = kill_offer_ctr;

  // Whole-queue observation: every position, zero where no entry lives, so the
  // vector is a total function of the state and can be compared every cycle.
  always_comb begin
    for (int unsigned j = 0; j < ENTRIES; j++) begin
      o_occ[j]                          = (j < n_wide);
      o_exc_occ[j]                      = (j < n_wide) && PayExc(ent[j]);
      o_entry_pay[j*ENTRY_W +: ENTRY_W] = (j < n_wide) ? ent[j] : {ENTRY_W{1'b0}};
    end
  end

endmodule : mosaic_result_fifo

`resetall
`default_nettype wire
