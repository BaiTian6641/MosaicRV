// ============================================================================
// mosaic_rob -- work package I-016: the architectural reorder buffer with
// per-macro (per-instruction) completion tracking.
//
// Sized entirely from the generated configuration package. Nothing here names a
// depth: MOSAIC_ROB_ENTRIES / MOSAIC_ROB_INDEX_W / MOSAIC_MAX_UOPS_PER_MACRO are
// the geometry file's values, so a profile change changes the hardware and the
// testbench's shadow model together and there is no second copy to drift.
// There is deliberately no depth parameter with an independent default: a
// caller-supplied default would be a second geometry, which is precisely the
// defect this package must not introduce.
//
// ------------------------------------------------ what one entry holds, and why
//
// The buffer holds one descriptor per **macro-uop**, i.e. per architectural
// instruction, not per micro-operation. A macro that expands into N children
// occupies exactly one slot and carries a completion bitmap of N bits. This is
// the design trade the card names -- "descriptor/bitmap replaces a bare last-uop
// signal" -- and the reason is a correctness argument:
//
//   An expansion of three children that completes in the order 2, 0, 1 does not
//   have its last *arriving* child as its last *completing* child. Any design
//   that retires a macro when the last-to-arrive child reports is therefore
//   wrong in the common case, and it is wrong silently: the ROB head advances
//   over two children that are still outstanding, and their results are written
//   into architectural state nobody will ever read them for. Retirement is
//   therefore a predicate over the whole bitmap, never over an arrival count or
//   an arrival index:
//
//       complete  <=>  done_mask == expected_mask(num_uops)
//
//   `expected_mask` is derived from the descriptor's own child count, so a
//   descriptor can never wait for a child that does not exist and can never be
//   satisfied by a child from outside its own expansion.
//
// -------------------------------------------------------- identity and wrap
//
// ROB_ENTRIES = 64 and the allocation pointer is a 6-bit circular index, so slot
// 63 is followed by slot 0 **every 64 allocations**. An index alone therefore
// cannot identify an in-flight macro: after the wrap, index 0 names a different
// macro than it did a moment ago, and a completion still in flight for the old
// macro 0 is delivered into the new macro 0. That is not a lost packet, it is a
// wrong result written into a live instruction.
//
// Every slot therefore carries a **generation**, and a completion names both:
// `(index, generation)`. The generation comes from a single monotonically
// increasing counter that advances on every allocation and is *never rewound* --
// not by reset, and not by flush. So the macro living in a slot now always has
// a generation different from the one any in-flight completion for a previous
// occupant of that slot carried, and that completion is rejected and *reported*
// rather than absorbed.
//
// The counter is GEN_W bits wide, so the guarantee holds for 2**GEN_W
// allocations; a completion that has been in flight longer than that, to the
// same slot, could alias. That window is a deliberate, documented bound rather
// than an accident -- 4096 allocations of ageing is far beyond anything a real
// recovery path leaves outstanding -- and it is the same trade every tag-based
// structure makes. It is derived from the entry count by the rule above, so a
// larger profile widens it without anyone editing a number.
//
// ------------------------------------------------------ the ports, in order
//
//   allocate   one macro descriptor per cycle. `alloc_num_uops` is the child
//              count of the expansion, so the expected bitmap is known from the
//              moment the slot is written and the descriptor arrives **closed**.
//              `alloc_open` allocates it open instead, for a caller that streams
//              children and closes the macro with the `close` port when the last
//              of them has been dispatched. An open macro accumulates
//              completions but can never retire: the set of children it is
//              waiting for is not final, and retiring on a bitmap that is still
//              growing is the same failure as retiring on an arrival count.
//
//   close      marks one macro's expansion final. Also generation-checked: a
//              close aimed at a recycled slot is reported stale, not applied.
//
//   complete   one child completion per cycle, naming `(index, generation,
//              uop)` and optionally raising an exception. Every outcome is
//              reported, and the outcomes are mutually exclusive:
//                  cmp_accepted  the bit was newly set
//                  cmp_duplicate the bit was already set -- the same child
//                                arriving twice. Detected, never absorbed: a
//                                silently absorbed duplicate is how an
//                                arrival-count design invents a completion.
//                  cmp_stale     the slot is not live, or its generation is not
//                                the one this completion names
//                  cmp_bad_uop   the uop index is outside this macro's own
//                                child count
//
//   retire     one in-order pop of the head, accepted only when the head is
//              complete, not exceptional and closed. `head_ready` says so
//              combinationally; `retire_ack` says whether the request was taken.
//
//   flush      drops every speculative macro -- everything at and above the head
//              -- and leaves committed history alone. The architectural history
//              of a ROB is its **counters**: how many macros were allocated,
//              retired and squashed. None of them moves on a flush. Nor does
//              the generation counter rewind, which is what keeps a pre-flush
//              completion in flight rejectable *across* the flush.
//
// -------------------------------------------------------- reset and storage
//
// `rst` is synchronous and active high. It clears the pointers, the occupancy,
// the conservation counters, the generation counter and the valid bits. It
// clears **nothing else**: the tag, PC, child count, bitmap, exception and
// closed bits are descriptor data, and clearing ROB_ENTRIES of them would give
// every slot a full reset, which is the rule rtl/common/mosaic_ram.sv documents
// against. Validity is held outside the descriptor arrays in an explicit packed
// vector, exactly as the RAM's worked example prescribes.
//
// The descriptor arrays are **not** RAMs and are not meant to infer as RAM: one
// slot is written by an allocation, written again by any number of child
// completions and by a close, and read by the head view and the observation port
// in arbitrary order. That is multi-port register state. The head view and the
// observation port are therefore combinational reads of register state, which is
// what the silicon does: there is no read latency for a consumer to model and
// no read-during-write mode to specify. This module is not a "RAM with a port
// bolted on" and does not claim to behave like one.
//
// Because reset is synchronous, the outputs still show the pre-reset state
// during the reset cycle itself and are clean from the first cycle after the
// reset edge.
//
// --------------------------------------------------------------- conservation
//
// Three counters are exported so the invariant is checkable rather than asserted
// in prose:
//
//     alloc_total == retired_total + squashed_total + occupied
//
// Every allocated macro is accounted for exactly once: retired, squashed, or
// still held. `occupied` is additionally the population count of the valid
// vector, which is the other half of the same fact.
//
// ------------------------------------------------------------------ mutants
//
// -DMOSAIC_ROB_MUTANT_<n> injects one defect used to prove the case can fail.
// The shipping build defines none of them; the table with real output is in
// results/reports/I-016-rob.md.
// ============================================================================

`default_nettype none
`resetall

/* verilator lint_off UNUSEDPARAM */
// The generated header declares one localparam per configuration knob for the
// whole project. This module names four of them; the rest belong to other
// modules and are unused *here* by construction, not by omission.
`include "mosaic_cfg_pkg.svh"
/* verilator lint_on UNUSEDPARAM */

module mosaic_rob #(
    parameter int unsigned ROB_ENTRIES = mosaic_cfg_pkg::MOSAIC_ROB_ENTRIES,
    parameter int unsigned ROB_INDEX_W = mosaic_cfg_pkg::MOSAIC_ROB_INDEX_W,
    parameter int unsigned MAX_UOPS    = mosaic_cfg_pkg::MOSAIC_MAX_UOPS_PER_MACRO,
    parameter int unsigned XLEN        = mosaic_cfg_pkg::MOSAIC_XLEN,

    // Derived, and therefore not overridable: a caller can change ROB_ENTRIES
    // or MAX_UOPS but can never create a width that disagrees with the geometry
    // it describes. Same convention as mosaic_ram's ADDR_WIDTH.
    localparam int unsigned CNT_W = $clog2(MAX_UOPS + 1),   // a count of 1..MAX_UOPS
    localparam int unsigned UOP_W = (MAX_UOPS <= 1) ? 1 : $clog2(MAX_UOPS),
    localparam int unsigned OCC_W = $clog2(ROB_ENTRIES + 1), // occupancy says "full" too

    // The two identity fields are sized by rule rather than by a number written
    // here, so the testbench wrapper can reproduce the same widths from the same
    // generated package instead of carrying a second copy of them:
    //
    //   TAG_W  the producer's name for an instruction. It only has to be unique
    //          among the <= ROB_ENTRIES macros in flight, so twice the slot
    //          index width is a 64x margin over the live population -- 12 bits
    //          for the 64-entry p0 ROB.
    //   GEN_W  the per-slot generation. Twice the slot index width gives
    //          2**GEN_W = 4096 allocations of aliasing margin for p0: a
    //          completion aimed at a slot cannot name a generation that macro's
    //          current occupant carries until 4096 further allocations have
    //          happened, which is far beyond anything a recovery path leaves
    //          outstanding. It is a deliberate, documented bound rather than an
    //          accident.
    localparam int unsigned TAG_W = 2 * ROB_INDEX_W,
    localparam int unsigned GEN_W = 2 * ROB_INDEX_W
) (
    input  logic                     clk,
    input  logic                     rst,

    // -------------------------------------------------------------- allocate
    input  logic                     alloc_valid,
    input  logic [TAG_W-1:0]         alloc_tag,
    input  logic [XLEN-1:0]          alloc_pc,
    input  logic [CNT_W-1:0]         alloc_num_uops,  // 1..MAX_UOPS
    input  logic                     alloc_exc,       // exception known at dispatch
    input  logic                     alloc_open,      // expansion is still streaming
    output logic                     alloc_ok,        // this allocation happened
    output logic                     alloc_refused,   // a valid allocation was refused
    output logic                     alloc_full,      // the ROB is at capacity
    output logic                     alloc_bad_uops,  // num_uops outside 1..MAX_UOPS
    output logic [ROB_INDEX_W-1:0]   alloc_index,     // identity of the new macro
    output logic [GEN_W-1:0]         alloc_gen,

    // ----------------------------------------------------------------- close
    input  logic                     close_valid,
    input  logic [ROB_INDEX_W-1:0]   close_index,
    input  logic [GEN_W-1:0]         close_gen,
    output logic                     close_ok,        // the expansion is now final
    output logic                     close_stale,     // ... aimed at a dead slot

    // --------------------------------------------------------------- complete
    input  logic                     cmp_valid,
    input  logic [ROB_INDEX_W-1:0]   cmp_index,
    input  logic [GEN_W-1:0]         cmp_gen,
    input  logic [UOP_W-1:0]         cmp_uop,
    input  logic                     cmp_exc,         // exception raised by this child
    output logic                     cmp_accepted,
    output logic                     cmp_duplicate,
    output logic                     cmp_stale,
    output logic                     cmp_bad_uop,

    // ---------------------------------------------------------------- retire
    //
    // `retire_req` / `retire_ack` are lane 0. The pair `retire_req_next` /
    // `retire_ack_next`, and the `head1_*` view, are the **second lane of the
    // same in-order pop**, added by I-017 because MOSAIC_RETIRE_WIDTH is 2 and a
    // buffer that can only pop one entry per cycle caps retirement at one
    // instruction per cycle no matter what decides eligibility.
    //
    // Lane 1 is not a second, independent decision point. It is the entry
    // immediately behind the head, qualified by **the same predicate as the
    // head** -- `head1_ready = head1_valid && head1_complete && !head1_exc &&
    // head1_closed` -- and `retire_ack_next` additionally requires lane 0 to be
    // acknowledged in the same cycle. So the second lane can only ever extend a
    // pop that is already happening; it can never start one, and it can never
    // reach past a blocked head. A design that consulted `obs_*` from a retire
    // consumer would be a functional read of a port this file documents as a
    // verification surface, which is why the view is exported properly here
    // instead.
    //
    // Both lanes are optional in the sense that a caller which drives
    // `retire_req_next` low is unaffected: every `head1_*` field is total and
    // gated on validity, and lane 1's state update is conditioned on
    // `retire_ack_next`.
    input  logic                     retire_req,
    output logic                     retire_ack,
    input  logic                     retire_req_next,
    output logic                     retire_ack_next,
    output logic                     head_valid,
    output logic                     head_ready,      // complete, clean and closed
    output logic                     head_replay,     // exceptional: replay here
    output logic                     head_complete,
    output logic                     head_exc,
    output logic                     head_closed,
    output logic [ROB_INDEX_W-1:0]   head_index,
    output logic [GEN_W-1:0]         head_gen,
    output logic [TAG_W-1:0]         head_tag,
    output logic [XLEN-1:0]          head_pc,
    output logic [CNT_W-1:0]         head_num_uops,
    output logic [MAX_UOPS-1:0]      head_done_mask,
    output logic [CNT_W-1:0]         head_done_cnt,
    output logic                     head1_valid,
    output logic                     head1_ready,     // the same predicate, one slot on
    output logic                     head1_replay,
    output logic                     head1_complete,
    output logic                     head1_exc,
    output logic                     head1_closed,
    output logic [ROB_INDEX_W-1:0]   head1_index,
    output logic [GEN_W-1:0]         head1_gen,
    output logic [TAG_W-1:0]         head1_tag,
    output logic [XLEN-1:0]          head1_pc,
    output logic [CNT_W-1:0]         head1_num_uops,
    output logic [MAX_UOPS-1:0]      head1_done_mask,
    output logic [CNT_W-1:0]         head1_done_cnt,

    // ----------------------------------------------------------------- flush
    input  logic                     flush_valid,

    // ----------------------------------------------------------- observation
    // A combinational view of one slot by index. The unit test uses it to check
    // every slot of the ROB, not only the head. A functional consumer has no
    // business reading it: the head view is the ROB's interface, and this is a
    // verification and debug port.
    input  logic [ROB_INDEX_W-1:0]   obs_index,
    output logic                     obs_valid,
    output logic [GEN_W-1:0]         obs_gen,
    output logic [TAG_W-1:0]         obs_tag,
    output logic [XLEN-1:0]          obs_pc,
    output logic [CNT_W-1:0]         obs_num_uops,
    output logic [MAX_UOPS-1:0]      obs_done_mask,
    output logic [CNT_W-1:0]         obs_done_cnt,
    output logic                     obs_exc,
    output logic                     obs_closed,

    // ----------------------------------------------------------------- status
    output logic [ROB_INDEX_W-1:0]   o_head_ptr,
    output logic [ROB_INDEX_W-1:0]   o_alloc_ptr,
    output logic [OCC_W-1:0]         o_occupied,
    output logic [OCC_W-1:0]         o_free,
    output logic [31:0]              o_alloc_total,
    output logic [31:0]              o_retired_total,
    output logic [31:0]              o_squashed_total,
    output logic [31:0]              o_gen_counter
);

  // ------------------------------------------------------------------ guards
  // $clog2 can name indices past a smaller ROB, so an entry count that does not
  // fill the index space is allowed, but every index is range-checked before it
  // reaches an array. For the shipping geometry the check is a tautology, so it
  // is not elaborated at all: emitting a comparison Verilator can prove constant
  // is exactly the dead logic -Wall exists to report.
  localparam bit IDX_NEVER_TRUNCATES = (ROB_ENTRIES == (1 << ROB_INDEX_W));

  logic [ROB_INDEX_W-1:0] cmp_idx;
  logic [ROB_INDEX_W-1:0] close_idx;

  generate
    if (IDX_NEVER_TRUNCATES) begin : g_idx_never_truncates
      assign cmp_idx   = cmp_index;
      assign close_idx = close_index;
    end else begin : g_idx_may_truncate
      assign cmp_idx   = (cmp_index   < ROB_INDEX_W'(ROB_ENTRIES)) ? cmp_index
                                                                  : {ROB_INDEX_W{1'b0}};
      assign close_idx = (close_index < ROB_INDEX_W'(ROB_ENTRIES)) ? close_index
                                                                   : {ROB_INDEX_W{1'b0}};
    end

    // An entry count the index cannot name at all is a configuration
    // contradiction, not a runtime condition. Referencing a module that does
    // not exist is an elaboration-time error; the branch is never elaborated.
    if (ROB_ENTRIES > (1 << ROB_INDEX_W)) begin : g_bad_entry_count
      mosaic_rob_contract_violation u_entry_count();
    end
    if (MAX_UOPS <= 0) begin : g_bad_max_uops
      mosaic_rob_contract_violation u_max_uops();
    end
  endgenerate

  // --------------------------------------------------------------- helpers
  // The circular increment is an explicit comparison, never a natural wrap of a
  // narrower counter, so a ROB_ENTRIES that is not a power of two aliases
  // nothing.
  localparam logic [ROB_INDEX_W-1:0] LAST_IDX = ROB_INDEX_W'(ROB_ENTRIES - 1);

  function automatic logic [ROB_INDEX_W-1:0] NextIdx(input logic [ROB_INDEX_W-1:0] idx);
    NextIdx = (idx == LAST_IDX) ? {ROB_INDEX_W{1'b0}} : (idx + ROB_INDEX_W'(1));
  endfunction

  // The set of children a descriptor waits for: the low `n` bits, for a count
  // in 1..MAX_UOPS. Written as a shift rather than as a mask subtraction so it
  // cannot produce a width-mismatched result for any MAX_UOPS.
  function automatic logic [MAX_UOPS-1:0] ExpectedMask(input logic [CNT_W-1:0] n);
    ExpectedMask = {MAX_UOPS{1'b1}} >> (CNT_W'(MAX_UOPS) - n);
  endfunction

  function automatic logic [CNT_W-1:0] PopCount(input logic [MAX_UOPS-1:0] mask);
    logic [CNT_W-1:0] count;
    count = {CNT_W{1'b0}};
    for (int unsigned i = 0; i < MAX_UOPS; i++) begin
      if (mask[i]) count = count + CNT_W'(1);
    end
    return count;
  endfunction

  // ------------------------------------------------------------------- state
  // Descriptor data. Deliberately not reset: see the reset contract above.
  logic [GEN_W-1:0]       slot_gen    [0:ROB_ENTRIES-1];
  logic [TAG_W-1:0]       slot_tag    [0:ROB_ENTRIES-1];
  logic [XLEN-1:0]        slot_pc     [0:ROB_ENTRIES-1];
  logic [CNT_W-1:0]       slot_count  [0:ROB_ENTRIES-1];
  logic [MAX_UOPS-1:0]    slot_done   [0:ROB_ENTRIES-1];
  logic                   slot_exc    [0:ROB_ENTRIES-1];
  logic                   slot_closed [0:ROB_ENTRIES-1];

  // Validity lives outside the descriptor arrays, one bit per slot.
  logic [ROB_ENTRIES-1:0] slot_valid;

  logic [ROB_INDEX_W-1:0] head_ptr;
  logic [ROB_INDEX_W-1:0] alloc_ptr;
  logic [OCC_W-1:0]       occ_cnt;

  // Conservation counters. All monotonic for the life of the machine; the
  // generation counter is monotonic across a flush as well.
  logic [31:0] alloc_total;
  logic [31:0] retired_total;
  logic [31:0] squashed_total;
  logic [31:0] gen_counter;

  // --------------------------------------------------------------- allocate
  assign alloc_full     = (occ_cnt == OCC_W'(ROB_ENTRIES));
  assign alloc_bad_uops = (alloc_num_uops == {CNT_W{1'b0}}) ||
                          (alloc_num_uops > CNT_W'(MAX_UOPS));

  // A flush has priority over an allocation offered in the same cycle: that
  // macro was speculative and the flush decides its fate, so admitting it would
  // leave a macro that is both squashed and alive.
`ifdef MOSAIC_ROB_MUTANT_NO_FULL_CHECK
  // NEGATIVE CONTROL 3: capacity is not consulted, so an allocation into a full
  // ROB overwrites the slot at alloc_ptr -- which, at capacity, is the oldest
  // live macro -- and the occupancy runs past ROB_ENTRIES.
  assign alloc_ok      = alloc_valid && !alloc_bad_uops && !flush_valid;
`else
  assign alloc_ok      = alloc_valid && !alloc_full && !alloc_bad_uops && !flush_valid;
`endif
  // `alloc_refused` reports a *resource or format* refusal, not a flush: an
  // allocation discarded by a flush is not something the producer could have
  // avoided, and counting it as a refusal would make the port's credit
  // accounting describe events that never happened.
  assign alloc_refused = alloc_valid && !flush_valid && (alloc_full || alloc_bad_uops);
  assign alloc_index   = alloc_ptr;
  assign alloc_gen     = gen_counter[GEN_W-1:0];

  // ------------------------------------------------------------------ close
  assign close_ok    = close_valid && !flush_valid && slot_valid[close_idx] &&
                       (slot_gen[close_idx] == close_gen);
  assign close_stale = close_valid && !flush_valid &&
                       !(slot_valid[close_idx] && (slot_gen[close_idx] == close_gen));

  // --------------------------------------------------------------- complete
  // Identity first: a completion aimed at a dead slot, or at a slot that now
  // holds a different macro, says nothing about child indices, because its
  // addressing is not about this macro at all.
  logic cmp_slot_live;
  logic cmp_gen_match;
  logic cmp_uop_in_range;
  logic cmp_already_set;
  logic cmp_identifies;

  assign cmp_slot_live = slot_valid[cmp_idx];
  assign cmp_gen_match = (slot_gen[cmp_idx] == cmp_gen);

`ifdef MOSAIC_ROB_MUTANT_NO_GEN_CHECK
  // NEGATIVE CONTROL 2: identity is decided by liveness alone. A completion
  // aimed at a recycled slot is then accepted as a child completion of whatever
  // macro now occupies that slot: a wrong result written into a live
  // instruction, and the ROB reports nothing.
  assign cmp_identifies = cmp_slot_live;
`else
  assign cmp_identifies = cmp_slot_live && cmp_gen_match;
`endif

  // The child count is compared in a width that holds both operands without
  // truncation. Truncating the count to the child-index width would make a
  // MAX_UOPS-child macro compare as zero children and reject every completion.
  localparam int unsigned CMP_W = (CNT_W > UOP_W) ? CNT_W : UOP_W;
  logic [CMP_W-1:0] cmp_uop_ext;
  logic [CMP_W-1:0] slot_count_ext;

  assign cmp_uop_ext      = CMP_W'(cmp_uop);
  assign slot_count_ext   = CMP_W'(slot_count[cmp_idx]);
  assign cmp_uop_in_range = cmp_uop_ext < slot_count_ext;
  assign cmp_already_set  = slot_done[cmp_idx][cmp_uop];

`ifdef MOSAIC_ROB_MUTANT_NO_DUP_REPORT
  // NEGATIVE CONTROL 5: the duplicate case is absorbed silently. Not counting
  // it twice is correct, but not reporting it means a producer that delivers
  // two results for one child is indistinguishable from one that does not, and
  // "detected" has to be an output to count as detection.
  assign cmp_duplicate = {1'b0};
`else
  assign cmp_duplicate = cmp_valid && !flush_valid && cmp_identifies &&
                         cmp_uop_in_range && cmp_already_set;
`endif

  assign cmp_accepted = cmp_valid && !flush_valid && cmp_identifies &&
                        cmp_uop_in_range && !cmp_already_set;
  assign cmp_stale    = cmp_valid && !flush_valid && !cmp_identifies;
  assign cmp_bad_uop  = cmp_valid && !flush_valid && cmp_identifies &&
                        !cmp_uop_in_range;

  // -------------------------------------------------------------- head view
  // Gated on `head_valid` for the same reason the observation view is: an empty
  // ROB has no descriptor at the head, and reading one would report whatever
  // the silicon powered up with. The gate costs the head no latency -- the view
  // is combinational either way -- and it makes every head field total, which is
  // what lets the testbench compare it on *every* cycle rather than only when
  // the ROB happens to be non-empty.
  logic [MAX_UOPS-1:0] head_raw_done;

  assign head_raw_done  = slot_done[head_ptr] &
                          ExpectedMask(slot_count[head_ptr]);
  assign head_done_mask = head_valid ? head_raw_done : {MAX_UOPS{1'b0}};

`ifdef MOSAIC_ROB_MUTANT_COMPLETE_ON_ANY_BIT
  // NEGATIVE CONTROL 1: any child at all makes the macro complete. This is the
  // card's "last-uop-arrives-is-treated-as-complete" failure in its most compact
  // form: a three-child macro whose child 2 happens to report first is retired
  // with children 0 and 1 still outstanding.
  assign head_complete  = head_valid && (head_raw_done != {MAX_UOPS{1'b0}});
`else
  assign head_complete  = head_valid &&
                          (head_raw_done == ExpectedMask(slot_count[head_ptr]));
`endif

  assign head_done_cnt  = PopCount(head_done_mask);
  assign head_valid     = slot_valid[head_ptr];
  assign head_index     = head_ptr;
  assign head_gen       = head_valid ? slot_gen[head_ptr]   : {GEN_W{1'b0}};
  assign head_tag       = head_valid ? slot_tag[head_ptr]   : {TAG_W{1'b0}};
  assign head_pc        = head_valid ? slot_pc[head_ptr]    : {XLEN{1'b0}};
  assign head_num_uops  = head_valid ? slot_count[head_ptr] : {CNT_W{1'b0}};
  assign head_exc       = head_valid && slot_exc[head_ptr];
  assign head_closed    = head_valid && slot_closed[head_ptr];

`ifdef MOSAIC_ROB_MUTANT_RETIRE_OVER_EXCEPTION
  // NEGATIVE CONTROL 6: an exceptional macro retires like any other, so the
  // head advances over the exacting instruction instead of blocking on it.
  assign head_ready     = head_valid && head_complete && head_closed;
`else
  assign head_ready     = head_valid && head_complete && !head_exc && head_closed;
`endif

  assign head_replay    = head_valid && head_exc;
  assign retire_ack     = retire_req && head_ready && !flush_valid;

  // ------------------------------------------------ second head view (I-017)
  // The entry immediately behind the head, qualified by **the same predicate**:
  // the same bitmap test, the same child-count mask, the same popcount, the same
  // validity gate. It is not a restatement -- a restatement is exactly how two
  // definitions of "done" come to exist, and the second one is the one that
  // drifts out of step with the first.
  logic [ROB_INDEX_W-1:0] head1_ptr;
  logic [MAX_UOPS-1:0]    head1_raw_done;

  assign head1_ptr       = NextIdx(head_ptr);
  assign head1_raw_done  = slot_done[head1_ptr] &
                           ExpectedMask(slot_count[head1_ptr]);
  assign head1_done_mask = head1_valid ? head1_raw_done : {MAX_UOPS{1'b0}};
  assign head1_done_cnt  = PopCount(head1_done_mask);
  assign head1_valid     = slot_valid[head1_ptr];
  assign head1_index     = head1_ptr;
  assign head1_gen       = head1_valid ? slot_gen[head1_ptr]   : {GEN_W{1'b0}};
  assign head1_tag       = head1_valid ? slot_tag[head1_ptr]   : {TAG_W{1'b0}};
  assign head1_pc        = head1_valid ? slot_pc[head1_ptr]    : {XLEN{1'b0}};
  assign head1_num_uops  = head1_valid ? slot_count[head1_ptr] : {CNT_W{1'b0}};
  assign head1_complete  = head1_valid &&
                           (head1_raw_done == ExpectedMask(slot_count[head1_ptr]));
  assign head1_exc       = head1_valid && slot_exc[head1_ptr];
  assign head1_closed    = head1_valid && slot_closed[head1_ptr];
  assign head1_replay    = head1_valid && slot_exc[head1_ptr];
  assign head1_ready     = head1_valid && head1_complete && !head1_exc &&
                           head1_closed;

  // Lane 1 rides **on top of** lane 0. `retire_ack` is the statement that the
  // head is actually leaving this cycle; without it the "second" entry is just
  // an unrelated slot, and acknowledging it would punch a hole in the queue.
  assign retire_ack_next = retire_req_next && retire_ack && head1_ready && !flush_valid;

  // ------------------------------------------------------- observation view
  // The same shape as the head view, over an arbitrary index, and gated on
  // validity: a slot nobody has written reports a zeroed descriptor rather than
  // whatever the silicon powered up with, so the view is total.
  assign obs_valid     = slot_valid[obs_index];
  assign obs_gen       = obs_valid ? slot_gen[obs_index]   : {GEN_W{1'b0}};
  assign obs_tag       = obs_valid ? slot_tag[obs_index]   : {TAG_W{1'b0}};
  assign obs_pc        = obs_valid ? slot_pc[obs_index]    : {XLEN{1'b0}};
  assign obs_num_uops  = obs_valid ? slot_count[obs_index] : {CNT_W{1'b0}};
  assign obs_done_mask = obs_valid ? (slot_done[obs_index] &
                                     ExpectedMask(slot_count[obs_index]))
                                   : {MAX_UOPS{1'b0}};
  assign obs_done_cnt  = PopCount(obs_done_mask);
  assign obs_exc       = obs_valid && slot_exc[obs_index];
  assign obs_closed    = obs_valid && slot_closed[obs_index];

  // -------------------------------------------------------------- next state
  // Occupancy is computed once, from a single copy of the pre-edge value, with
  // explicit priority. One statement doing `occ_cnt + 1` and another doing
  // `occ_cnt - 1` would make the result depend on the simulator's ordering of
  // two non-blocking assignments to the same element -- correct in one
  // simulator and not in another, which is exactly the class of defect this
  // testbench cannot see.
  logic [OCC_W-1:0] occ_cnt_next;
  always_comb begin
    if (flush_valid) begin
      occ_cnt_next = {OCC_W{1'b0}};
    end else begin
      occ_cnt_next = occ_cnt;
      if (alloc_ok)         occ_cnt_next = occ_cnt_next + OCC_W'(1);
      // Lane 1's acknowledgement is a second, separate pop and is counted as
      // one: a two-wide retire consumes two slots and must decrement twice, or
      // the occupancy drifts up by one per two-wide retirement and the buffer
      // reports itself full while it is not.
      if (retire_ack)       occ_cnt_next = occ_cnt_next - OCC_W'(1);
      if (retire_ack_next)  occ_cnt_next = occ_cnt_next - OCC_W'(1);
    end
  end

  logic [ROB_INDEX_W-1:0] head_ptr_next;
  logic [ROB_INDEX_W-1:0] alloc_ptr_next;
  always_comb begin
    if (flush_valid) begin
      // Everything in flight is squashed, so the head of an empty ROB is the
      // tail: the next allocation reuses the slots the flush released, with
      // generations that the stale in-flight completions do not match.
      head_ptr_next  = alloc_ptr;
      alloc_ptr_next = alloc_ptr;
    end else begin
      // The head advances by exactly as many slots as were acknowledged. Two
      // explicit steps rather than an addition of the two ack bits, so the
      // increment function -- which is the one place a non-power-of-two entry
      // count could alias -- is applied to a value it has already been proven to
      // range-check.
      head_ptr_next  = head_ptr;
      if (retire_ack)      head_ptr_next = NextIdx(head_ptr_next);
      if (retire_ack_next) head_ptr_next = NextIdx(head_ptr_next);
      alloc_ptr_next = alloc_ok   ? NextIdx(alloc_ptr) : alloc_ptr;
    end
  end

  always_ff @(posedge clk) begin
    if (rst) begin
      // Control state only. Every descriptor array above is deliberately
      // untouched: clearing ROB_ENTRIES of them would give every slot a full
      // reset, which is precisely what this project's RAM rule forbids.
      slot_valid     <= {ROB_ENTRIES{1'b0}};
      head_ptr       <= {ROB_INDEX_W{1'b0}};
      alloc_ptr      <= {ROB_INDEX_W{1'b0}};
      occ_cnt        <= {OCC_W{1'b0}};
      alloc_total    <= 32'd0;
      retired_total  <= 32'd0;
      squashed_total <= 32'd0;
      gen_counter    <= 32'd0;
    end else begin
      if (flush_valid) begin
        slot_valid <= {ROB_ENTRIES{1'b0}};
      end else begin
        if (alloc_ok) begin
          slot_valid[alloc_index]  <= 1'b1;
          slot_gen[alloc_index]    <= alloc_gen;
          slot_tag[alloc_index]    <= alloc_tag;
          slot_pc[alloc_index]     <= alloc_pc;
          slot_count[alloc_index]  <= alloc_num_uops;
          slot_done[alloc_index]   <= {MAX_UOPS{1'b0}};
          slot_exc[alloc_index]    <= alloc_exc;
          // An allocation with `alloc_open` leaves the expansion unclosed: the
          // child count is known, but the producer has not said it has finished
          // describing the macro, and an unclosed macro cannot retire because
          // the set of children it waits for is not yet final.
          slot_closed[alloc_index] <= !alloc_open;
        end

        if (close_ok) begin
          slot_closed[close_idx] <= 1'b1;
        end

        if (cmp_accepted) begin
          slot_done[cmp_idx][cmp_uop] <= 1'b1;
          if (cmp_exc) slot_exc[cmp_idx] <= 1'b1;
        end

        // Both lanes clear a slot. Lane 1's slot is `head1_ptr`, captured from
        // the same pre-edge `head_ptr` this block already reads, so a two-wide
        // pop frees the head and the entry behind it in the one cycle they left.
        if (retire_ack) begin
          slot_valid[head_ptr] <= 1'b0;
        end
        if (retire_ack_next) begin
          slot_valid[head1_ptr] <= 1'b0;
        end
      end

      head_ptr  <= head_ptr_next;
      alloc_ptr <= alloc_ptr_next;
      occ_cnt   <= occ_cnt_next;

      if (alloc_ok) begin
        alloc_total <= alloc_total + 32'd1;
        gen_counter <= gen_counter + 32'd1;
      end
      // Two acks, two retirements: the conservation identity
      // `alloc == retired + squashed + occupied` is only meaningful if the
      // retirement counter and the occupancy move together, and lane 1 moves
      // both.
      if (retire_ack)       retired_total <= retired_total + 32'd1;
      if (retire_ack_next)  retired_total <= retired_total + 32'd1;
      // Explicitly widened rather than concatenated to 32 bits: OCC_W depends on
      // the entry count, so a fixed 24-bit pad is only the right width for a
      // 64-entry ROB and silently truncates for any other.
      if (flush_valid) squashed_total <= squashed_total + {{(32 - OCC_W) {1'b0}}, occ_cnt};

`ifdef MOSAIC_ROB_MUTANT_FLUSH_CLEARS_COMMITTED
      // NEGATIVE CONTROL 4: a flush also rewinds the committed history and the
      // generation counter. The rewound counter is the dangerous half: a
      // completion still in flight for a squashed macro then matches the
      // generation of whatever macro next lands in that slot, and is accepted
      // as a real child completion of it.
      if (flush_valid) begin
        retired_total <= 32'd0;
        gen_counter   <= 32'd0;
      end
`endif
    end
  end

  // ------------------------------------------------------------------ status
  assign o_head_ptr       = head_ptr;
  assign o_alloc_ptr      = alloc_ptr;
  assign o_occupied       = occ_cnt;
  assign o_free           = OCC_W'(ROB_ENTRIES) - occ_cnt;
  assign o_alloc_total    = alloc_total;
  assign o_retired_total  = retired_total;
  assign o_squashed_total = squashed_total;
  assign o_gen_counter    = gen_counter;

endmodule : mosaic_rob

`resetall
`default_nettype wire
