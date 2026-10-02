// ============================================================================
// mosaic_reservation -- work package I-040: the LR/SC reservation manager.
//
// ------------------------------------------------------------------ the state
//
// One hart, one reservation: a validity bit and the tag of the granule it
// covers. The **granule** is the naturally aligned block that contains the LR's
// address, and its size is a *platform declaration* rather than a free choice:
// `GRANULE_BITS` is the single place that size is stated. The ACT4 target this
// core is validated against declares it -- tests/act4/mosaic-p1/mosaic-p1.yaml
// sets `LRSC_RESERVATION_STRATEGY: "reserve exactly enough to cover the access"`
// (an XLEN-sized set) and the reference model's
// `platform.reservation.reservation_set_size_exp` is 3, i.e. 8 bytes, the
// minimum an RV64 Zalrsc implementation may declare. Reserving *more* than the
// declaration is architecturally permitted (an SC may fail for any reason, so
// the converse is a liberty), but it is observable in the other direction: an SC
// to an address the reference places outside the set succeeds here. That is a
// real divergence, not a liberty, and it is what the two `Zalrsc-sc.*` ELFs
// found -- so the set is the declared one, not a larger one. A future profile
// that advertises `Za64rs` declares a larger set and this constant is where it
// changes.
//
// --------------------------------------------------------- the invalidation set
//
// The reservation is established by an LR (`set_valid_i`) and is destroyed by
// every event the ISA makes it a function of:
//
//   * **any write this hart performs through the memory endpoint whose byte
//     range intersects the granule** (`own_write_valid_i`). That one strobe
//     covers all three writers: an ordinary store, the write beat of an AMO, and
//     the write of a *successful* SC. The endpoint asserts it on the offered
//     write beat, which is idempotent -- clearing an already-clear reservation
//     costs nothing, and clearing it a cycle early is conservative in the only
//     direction an SC may fail.
//   * **any write another agent performs to the granule**, delivered as a
//     notification (`ext_write_valid_i`). With no cache and one hart on the
//     memory port this is the coherence interface: a deeper design drives it
//     from the same snoop traffic that backs cache invalidations, and the case
//     drives it from the second agent it models. A notification the endpoint
//     cannot observe because the other agent is not in the RTL would be a
//     reservation that outlives a conflicting write -- precisely the failure the
//     card's "a reservation not cleared by an external write" control moves.
//   * **any SC this hart executes** (`consume_valid_i`), whether it succeeds or
//     fails. An SC pairs with the most recent LR, so it must not leave a
//     reservation behind for a second SC to pair with; that is what makes
//     "SC succeeds exactly once" a property rather than a coincidence.
//   * **an exception or a context switch** (`flush_valid_i`). Architecturally the
//     reservation is a property of the hart's current execution; it does not
//     survive a trap, and the case exercises an exception taken between the LR
//     and the SC.
//
// A write whose byte range does **not** intersect the granule does not clear it.
// The ISA permits an implementation to be more pessimistic -- an SC may fail for
// any reason -- but here over-invalidation would be an observable defect: it
// turns a store to an unrelated address into a spurious SC failure, and the
// card's fourth control (`MOSAIC_LRSC_MUTANT_GRANULE_OVERINVALIDATE`) is exactly
// the mutation that makes an out-of-granule write clear the reservation. The
// rule is stated once, in `touches`, and both write sources use it.
//
// A clear wins over a set in the same cycle. The LR's read has already happened
// when it establishes the reservation, so a write arriving in that same cycle is
// ordered *after* the read and must invalidate the reservation it would
// otherwise leave standing.
//
// ------------------------------------------------------------------ the check
//
// `hit_o` is combinational: an SC must decide whether it will write in the cycle
// it is accepted, and it must not be able to observe a reservation that a write
// has already destroyed. `check_valid_i` is the strobe that says an SC is being
// checked, so the hit/miss counters count decisions and not cycles.
// ============================================================================

`ifndef MOSAIC_RESERVATION_SV_
`define MOSAIC_RESERVATION_SV_

`default_nettype none
`resetall

/* verilator lint_off UNUSEDPARAM */
// The generated header declares one localparam per configuration knob for the
// whole project. This module names the subset it needs; the rest belong to other
// modules and are unused *here* by construction, not by omission.
`include "mosaic_cfg_pkg.svh"
/* verilator lint_on UNUSEDPARAM */

module mosaic_reservation (
    input  logic        clk,
    input  logic        rst,

    // ---------------------------------------------------------- establishment
    // A completed LR: set the reservation on the granule containing `set_addr_i`.
    input  logic        set_valid_i,
    input  logic [63:0] set_addr_i,

    // ------------------------------------------------------------- the check
    // Combinational: does the reservation cover `check_addr_i` right now?
    input  logic [63:0] check_addr_i,
    input  logic        check_valid_i,
    output logic        hit_o,

    // ------------------------------------------- this hart's own writes (any)
    // Asserted while a write beat is offered to the memory system, with the
    // address and the byte count of that write.
    input  logic        own_write_valid_i,
    input  logic [63:0] own_write_addr_i,
    input  logic [3:0]  own_write_bytes_i,

    // ----------------------------------------------- another agent's writes
    // The coherence notification: an address and byte count another master
    // wrote. Honoured on the same granule rule as this hart's own writes.
    input  logic        ext_write_valid_i,
    input  logic [63:0] ext_write_addr_i,
    input  logic [3:0]  ext_write_bytes_i,

    // ------------------------------------------------------------ the flush
    // An exception or a context switch: the reservation does not survive it.
    input  logic        flush_valid_i,

    // ---------------------------------------------------------- SC consumption
    // An SC is being accepted. It consumes the reservation whether or not it
    // will succeed.
    input  logic        consume_valid_i,

    // ------------------------------------------------------------ observability
    output logic        valid_o,
    output logic [63:0] granule_o,
    output logic [31:0] set_ctr_o,
    output logic [31:0] clear_ctr_o,
    output logic [31:0] clear_ext_ctr_o,
    output logic [31:0] hit_ctr_o,
    output logic [31:0] miss_ctr_o
);

  // 8 bytes -- the XLEN-sized set the platform declares (`GRANULE_BITS` is the
  // exponent of its size): the granule is `addr[63:3]` and the low three bits
  // are inside it. MOSAIC_LRSC_MUTANT_GRANULE_64B restores the oversized 64-byte
  // set the endpoint shipped with, which made an SC to an address the reference
  // declares outside the set succeed; CASE=core.act_dut's Zalrsc-sc.* ELFs name
  // it.
  `ifdef MOSAIC_LRSC_MUTANT_GRANULE_64B
    localparam int unsigned GRANULE_BITS = 6;
  `else
    localparam int unsigned GRANULE_BITS = 3;
  `endif
  localparam int unsigned GRANULE_W    = 64 - GRANULE_BITS;

  logic                valid_q;
  logic [GRANULE_W-1:0] granule_q;

  logic [31:0] set_ctr_q, clear_ctr_q, clear_ext_ctr_q, hit_ctr_q, miss_ctr_q;

  // The granule an address belongs to. One function, so the address compared at
  // establishment and the addresses compared at invalidation are always the same
  // slice of the same width. The low `GRANULE_BITS` bits are *inside* the granule
  // and are deliberately not read -- a granule tag names the block, not the byte
  // -- so the linter's "unused bits of the input" is the statement of the rule
  // rather than an omission.
  /* verilator lint_off UNUSEDSIGNAL */
  function automatic logic [GRANULE_W-1:0] granule_of(input logic [63:0] addr);
    begin
      granule_of = addr[63:GRANULE_BITS];
    end
  endfunction
  /* verilator lint_on UNUSEDSIGNAL */

  // Does the byte range `[addr, addr + bytes - 1]` intersect the reserved
  // granule? An access of at most eight bytes spans at most two granules, so
  // testing the granule of the first and of the last byte is exact -- and it is
  // exact for a range that starts outside the granule and ends inside it, which
  // a comparison of the first byte alone would miss.
  function automatic logic touches(input logic [63:0] addr, input logic [3:0] bytes);
    logic [63:0] last;
    begin
      last    = addr + {{60{1'b0}}, bytes} - 64'd1;
      touches = (granule_of(addr) == granule_q) || (granule_of(last) == granule_q);
    end
  endfunction

  logic own_touch_c;
  logic ext_touch_c;
`ifdef MOSAIC_LRSC_MUTANT_GRANULE_OVERINVALIDATE
  // NEGATIVE CONTROL: any write clears the reservation, whatever its address --
  // the over-invalidation that turns an out-of-granule store into a spurious SC
  // failure. CASE=lrsc.reservation_progress's "a store outside the granule
  // leaves the reservation standing" check names it.
  assign own_touch_c = own_write_valid_i;
  assign ext_touch_c = ext_write_valid_i;
`elsif MOSAIC_LRSC_MUTANT_NO_EXT_INVAL
  // NEGATIVE CONTROL: another agent's write never invalidates the reservation,
  // so a conflicting write is invisible and the SC succeeds on stale state.
  // CASE=lrsc.reservation_progress's "another agent's write to the granule
  // breaks the reservation" check names it.
  assign own_touch_c = own_write_valid_i && touches(own_write_addr_i, own_write_bytes_i);
  assign ext_touch_c = 1'b0;
`else
  assign own_touch_c = own_write_valid_i && touches(own_write_addr_i, own_write_bytes_i);
  assign ext_touch_c = ext_write_valid_i && touches(ext_write_addr_i, ext_write_bytes_i);
`endif

  logic clear_c;
  assign clear_c = flush_valid_i || consume_valid_i || own_touch_c || ext_touch_c;

  // Combinational, and a function of the registered state alone: an SC checked in
  // the same cycle a conflicting write is notified sees the reservation already
  // gone, because the notification is what clears it.
  assign hit_o = valid_q && (granule_of(check_addr_i) == granule_q);

  assign valid_o   = valid_q;
  assign granule_o = {granule_q, {GRANULE_BITS{1'b0}}};

  always_ff @(posedge clk) begin
    if (rst) begin
      valid_q         <= 1'b0;
      granule_q       <= {GRANULE_W{1'b0}};
      set_ctr_q       <= 32'd0;
      clear_ctr_q     <= 32'd0;
      clear_ext_ctr_q <= 32'd0;
      hit_ctr_q       <= 32'd0;
      miss_ctr_q      <= 32'd0;
    end else begin
      if (clear_c) begin
        valid_q     <= 1'b0;
        clear_ctr_q <= clear_ctr_q + 32'd1;
        if (ext_touch_c) clear_ext_ctr_q <= clear_ext_ctr_q + 32'd1;
      end else if (set_valid_i) begin
        valid_q <= 1'b1;
      end
      if (set_valid_i && !clear_c) begin
        granule_q <= granule_of(set_addr_i);
        set_ctr_q <= set_ctr_q + 32'd1;
      end
      if (check_valid_i) begin
        if (hit_o) hit_ctr_q  <= hit_ctr_q  + 32'd1;
        else       miss_ctr_q <= miss_ctr_q + 32'd1;
      end
    end
  end

  assign set_ctr_o       = set_ctr_q;
  assign clear_ctr_o     = clear_ctr_q;
  assign clear_ext_ctr_o = clear_ext_ctr_q;
  assign hit_ctr_o       = hit_ctr_q;
  assign miss_ctr_o      = miss_ctr_q;

endmodule

`endif  // MOSAIC_RESERVATION_SV_
