// ============================================================================
// mosaic_amo_unit -- work package I-039: the issue record for the one atomic
// read-modify-write the load queue is currently carrying.
//
// ------------------------------------------------------------------- why
//
// An AMO is issued through the load queue, and that is deliberate: the load
// queue is the only structure that (a) returns a value to a destination
// register before retirement and (b) *blocks younger loads behind an older
// unissued entry*, which is what stops a younger load to the same address from
// observing the pre-AMO value. An AMO issued any other way would be a coherence
// hole that p0 has no mechanism (I-036) to repair.
//
// But the load queue's entry cannot carry the operation, the ordering bits or
// the operand: `lsu_req_t` is a frozen interface and the load queue's ports are
// owned by CASE=load_queue.forwarding. So the fact the load queue cannot carry
// is carried here, keyed by the macro identity the load queue *does* present on
// its request port. The macro identity is exact (`rob_index` + `rob_gen` +
// `uop_index`, with the hart field), so the match cannot confuse two macros, and
// it cannot go stale inside a live queue entry.
//
// ------------------------------------------------------------- one at a time
//
// `busy` reports that a record is held. The core refuses to allocate a second
// AMO while one is held, so at most one AMO is resident in the load queue and
// the record needs exactly one entry. That is a throughput choice, not a
// correctness one: it is the smallest structure that keeps the record exact,
// and the load queue remains able to hold ordinary loads behind the AMO.
//
// The record lives from the cycle the load queue accepts the macro until the
// transaction is handed to the serialization point (`taken`) or the queue is
// flushed (`flush`). After `taken` the operation and operand are latched in the
// endpoint's own transaction register, so a second copy here would only be a
// second thing to keep in step.
// ============================================================================

`ifndef MOSAIC_AMO_UNIT_SV_
`define MOSAIC_AMO_UNIT_SV_

`default_nettype none
`resetall

/* verilator lint_off UNUSEDPARAM */
`include "mosaic_cfg_pkg.svh"
/* verilator lint_on UNUSEDPARAM */

module mosaic_amo_unit (
    input  logic                       clk,
    input  logic                       rst,

    // ---------------------------------------------------- staging (allocation)
    // The cycle the load queue accepts the AMO macro. `alloc_ready` (the
    // negation of `busy`) is what the core feeds into dispatch's readiness, so
    // these never arrive while a record is already held.
    input  logic                       alloc_valid_i,
    input  mosaic_uop_pkg::uop_id_t    alloc_id_i,
    input  mosaic_pkg::amo_op_e        alloc_op_i,
    input  logic                       alloc_aq_i,
    input  logic                       alloc_rl_i,
    // LR/SC (I-040). The same record holds the paired atomics: an LR carries no
    // operation and no operand (it reads and reserves), an SC carries the store
    // data in `alloc_operand_i`. Keeping all three in one record is what makes
    // "at most one atomic macro resident in the load queue" -- and therefore a
    // single-entry record -- a property of one structure rather than of three
    // that could each be holding one.
    input  logic                       alloc_is_lr_i,
    input  logic                       alloc_is_sc_i,
    input  logic [mosaic_cfg_pkg::MOSAIC_XLEN-1:0] alloc_operand_i,

    // The transaction has been handed to the serialization point, or the load
    // queue is being flushed; either way the record must go.
    input  logic                       taken_i,
    input  logic                       flush_i,

    // ------------------------------------------------------- lookup (issue)
    // The load queue's current request identity.
    input  mosaic_uop_pkg::uop_id_t    probe_id_i,
    output logic                       match_o,
    output mosaic_pkg::amo_op_e        op_o,
    output logic                       aq_o,
    output logic                       rl_o,
    output logic                       is_lr_o,
    output logic                       is_sc_o,
    output logic [mosaic_cfg_pkg::MOSAIC_XLEN-1:0] operand_o,

    // The identity and validity of the held record, published so the load queue
    // can recognise its *own* atomic head. An atomic macro must never be
    // satisfied by store-to-load forwarding: an LR that is completed from a
    // store never reaches memory and never establishes a reservation, and an SC
    // that is completed from a store never performs its write. The queue uses
    // this to force the atomic head to memory (`atomic_valid_o`/`atomic_id_o`).
    output mosaic_uop_pkg::uop_id_t    atomic_id_o,
    output logic                       atomic_valid_o,

    // The core refuses a further AMO allocation while this is high.
    output logic                       busy_o
);

  localparam int unsigned XLEN = mosaic_cfg_pkg::MOSAIC_XLEN;

  logic                    valid_q;
  mosaic_uop_pkg::uop_id_t id_q;
  mosaic_pkg::amo_op_e     op_q;
  logic                    aq_q, rl_q;
  logic                    is_lr_q, is_sc_q;
  logic [XLEN-1:0]         operand_q;

  assign busy_o = valid_q;

  // A combinatorial match against the (stable) held record. The request port's
  // payload is held stable while `valid && !ready` by the project-wide
  // transport rule, and the record is held stable until `taken`/`flush`, so the
  // match is stable for as long as it is needed.
  assign match_o   = valid_q && mosaic_uop_pkg::uop_id_eq(id_q, probe_id_i);
  assign op_o      = op_q;
  assign aq_o      = aq_q;
  assign rl_o      = rl_q;
  assign is_lr_o   = is_lr_q;
  assign is_sc_o   = is_sc_q;
  assign operand_o = operand_q;

  assign atomic_id_o    = id_q;
  assign atomic_valid_o = valid_q;

  always_ff @(posedge clk) begin
    if (rst) begin
      valid_q <= 1'b0;
      // The payload registers are deliberately not reset: they are read only
      // when `valid_q` is high, and `valid_q` is written by every allocation, so
      // a reset would be reset fanout holding a value nothing can observe (the
      // same trade rtl/common/mosaic_ram.sv documents).
    end else if (flush_i || taken_i) begin
      valid_q <= 1'b0;
    end else if (alloc_valid_i) begin
      valid_q   <= 1'b1;
      id_q      <= alloc_id_i;
      op_q      <= alloc_op_i;
      aq_q      <= alloc_aq_i;
      rl_q      <= alloc_rl_i;
      is_lr_q   <= alloc_is_lr_i;
      is_sc_q   <= alloc_is_sc_i;
      operand_q <= alloc_operand_i;
    end
  end

endmodule

`endif  // MOSAIC_AMO_UNIT_SV_
