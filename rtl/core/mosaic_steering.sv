// ============================================================================
// mosaic_steering -- the dynamic steering baseline policy (work package I-029).
//
// One macro is offered to this unit every cycle -- the macro dispatch is about
// to insert -- and it answers *which execution resource that macro goes to*.
// It is a pure decision unit: it owns no queue, no credit and no data path, and
// it can therefore be wrong about nothing except the choice. That property is
// the whole point of the package: routing may change *when* work executes and
// *where* it executes, and it must never change *what* the architectural result
// is.
//
// ---------------------------------------------------------------- the rule
//
// Four keys, applied in this order, all of them a function of the program's own
// state (the offer, the capability matrix and the machine's occupancy -- never
// of a free-running counter, a temperature, a random source or a performance
// estimate):
//
//   1. capability   A unit is eligible for a macro iff its capability mask has
//                   the macro's class. A unit that cannot execute a class is
//                   never a candidate, in any mode, under any load. A class no
//                   unit implements (p0's SYSTEM class: a CSR access, a trap
//                   instruction or a fence is resolved at the architectural
//                   boundary, not in a functional unit) is `reject`ed and is
//                   never granted to a unit at all.
//   2. locality     Among the eligible units with room, the *preferred* pool is
//                   those that are local to the macro's producer (`unit_locality
//                   == req_locality`) or are shared between localities. If the
//                   macro states no preference (`req_locality_en` low: its
//                   producer was a shared unit, or it has none) the preferred
//                   pool is every candidate, so the key discriminates nothing
//                   and the load key is what decides. If the preferred pool is
//                   empty the full eligible set is used: an operand produced in
//                   the other cluster is a longer wire, not an impossibility, so
//                   locality is a preference and never a filter that could
//                   starve a macro.
//   3. load         Among the pool, the least occupied unit wins. Occupancy is
//                   an input -- the unit's own queue occupancy -- and not the
//                   place a decision is guessed from.
//   4. age          Ties on occupancy are broken by *waiting time*, not by
//                   index: the unit whose last grant is oldest wins. The
//                   register `last_age` is written with the age of the macro a
//                   unit was granted, so the tie-break state is derived from the
//                   grants the program actually caused; a unit that was just fed
//                   goes behind one that has been idle since before the oldest
//                   macro in the machine. Two equal candidates with no history
//                   tie on the lowest index, which is the only remaining
//                   deterministic and history-free choice.
//
// `grant_reason` reports which key was the one that decided, so the decision is
// observable rather than implied: 0 none, 1 fixed affinity, 2 capability alone,
// 3 locality, 4 load, 5 age. Mode 0 (`mode_dyn` low) is the fixed baseline the
// card requires as the control: every class goes to the unit the fixed table
// names, load and locality are not consulted, and it declines -- by stalling --
// when that unit has no room, rather than spilling to another one. The two modes
// are the same machine with one strategy changed, which is what makes their
// comparison an experiment rather than two different designs.
//
// ------------------------------------------------------- age and its modulus
//
// `last_age` is compared unsigned, and that is exact only while the distance
// between two live ages is below half the modulus (the plan's section 1.3 rule:
// a scheduling age's modulus must exceed twice the largest comparable
// distance). `req_age` is the macro's program-order age and the stream this
// unit is measured over is bounded (the case's programs are at most 4096 macros
// and the driver states that bound), so with AGE_W = 16 the largest comparable
// distance is 4096 and the modulus 65536 is more than eight times the bound.
// Wrapping is therefore not reachable in this profile, and is stated here
// rather than left to be inferred from a width.
//
// ---------------------------------------------- what this unit deliberately is not
//
// It does not add a unit, widen a queue, or change a capacity to make a routing
// look better (the card's fail mode). The only state it owns is the age of each
// unit's last grant plus three counters; every capacity it sees arrives as an
// input. It does not estimate performance, decode an opcode it was not handed,
// or consult anything outside the offer and the machine's occupancy.
//
// ------------------------------------------------------------------ mutants
//
// Three `-DMOSAIC_STEER_MUTANT_*` defines inject one defect each. The shipping
// build defines none. The table with real output is in
// results/reports/I-029-steering.md:
//
//   IGNORE_CAPABILITY  the capability filter is dropped, so a class can be
//                      granted to a unit that cannot execute it.
//   SATURATION_ADMIT   room is not consulted, so saturation is "resolved" by
//                      admitting a macro into a full unit -- the dropped-uop
//                      fail mode -- instead of stalling it.
//   NONDET_AGE         the age tie-break reads a counter `rst` does not clear
//                      instead of the grant history, so the decision stops
//                      being a function of the program's state.
// ============================================================================

`default_nettype none
`resetall

/* verilator lint_off UNUSEDPARAM */
`include "mosaic_cfg_pkg.svh"
`include "mosaic_id_pkg.svh"
/* verilator lint_on UNUSEDPARAM */
`include "mosaic_pkg.sv"
`include "mosaic_uop_pkg.sv"

module mosaic_steering #(
    // Four targets: the two clusters' integer ALUs and the two shared units
    // (MUL/DIV and the LSU), which is the geometry of rtl/core/mosaic_core.sv.
    parameter int unsigned ST_N_UNITS   = 4,
    // One bit per class of mosaic_uop_pkg::uop_class_e.
    parameter int unsigned ST_N_CLASSES = 6,
    parameter int unsigned ST_AGE_W     = 16,
    parameter int unsigned ST_OCC_W     = 4,
    parameter int unsigned ST_UNIT_W    = (ST_N_UNITS <= 1) ? 1 : $clog2(ST_N_UNITS)
) (
    input  logic                      clk,
    input  logic                      rst,

    // 0 = the fixed-affinity baseline, 1 = locality/load/age steering.
    input  logic                      mode_dyn,

    // ----------------------------------------------------- the macro offered
    input  logic                      req_valid,
    input  mosaic_uop_pkg::uop_class_e req_class,
    // The cluster the operand's producer ran in: 0 or 1. `req_locality_en` low
    // means there is no preference to express -- the producer was a shared unit
    // (which belongs to neither cluster) or the macro has no producer at all --
    // and then locality does not narrow the pool.
    input  logic                      req_locality,
    input  logic                      req_locality_en,
    // The macro's program-order age. It is recorded on a grant and is the only
    // thing the age tie-break compares.
    input  logic [ST_AGE_W-1:0]       req_age,

    // --------------------------------------------------- the machine's state
    // Room for one more entry, per unit.
    input  logic [ST_N_UNITS-1:0]              unit_room,
    // The unit's occupancy, per unit; the least-loaded key.
    input  logic [ST_N_UNITS*ST_OCC_W-1:0]     unit_occ,
    // The capability matrix: bit `u*ST_N_CLASSES + c` says unit `u` may execute
    // a macro of class `c`. It is an input, so the matrix has one owner.
    input  logic [ST_N_UNITS*ST_N_CLASSES-1:0] unit_cap,
    // Which cluster each unit belongs to, and whether it serves both.
    input  logic [ST_N_UNITS-1:0]              unit_locality,
    input  logic [ST_N_UNITS-1:0]              unit_shared,
    // The fixed baseline's table: the unit each class is pinned to.
    input  logic [ST_N_CLASSES*ST_UNIT_W-1:0]  fixed_unit,

    // ------------------------------------------------------------- decision
    output logic                      grant_valid,
    output logic [ST_UNIT_W-1:0]      grant_unit,
    output logic [2:0]                grant_reason,
    // The offer was refused and is retried with the same identity next cycle:
    // in the dynamic strategy no unit that could take this class has room, and
    // in the fixed baseline the pinned unit has none.
    output logic                      stall,
    // No unit in the machine implements this class at all.
    output logic                      reject,

    // ---------------------------------------------------------- observation
    output logic [ST_N_UNITS*32-1:0]  o_unit_issues,
    output logic [31:0]               o_grant_ctr,
    output logic [31:0]               o_stall_ctr,
    output logic [31:0]               o_reject_ctr,

    // Geometry read-back, so a driver never hardcodes this module's widths.
    output logic [31:0]               o_units,
    output logic [31:0]               o_classes,
    output logic [31:0]               o_age_w,
    output logic [31:0]               o_unit_w,
    output logic [31:0]               o_occ_w
);

  localparam logic [ST_AGE_W-1:0] ST_AGE_ZERO = {ST_AGE_W{1'b0}};
  localparam logic [31:0]         ST_ONE      = 32'd1;

  // The age of each unit's last grant. This is the entire tie-break state.
  logic [ST_AGE_W-1:0] last_age [ST_N_UNITS];
  // The value the age key compares: the grant history in the shipping build.
  logic [ST_AGE_W-1:0] age_ref [ST_N_UNITS];

  logic [ST_N_UNITS-1:0] cap_ok;
  logic [ST_N_UNITS-1:0] cand;
  logic [ST_N_UNITS-1:0] pref;
  logic [ST_N_UNITS-1:0] use_set;
  logic [ST_N_UNITS-1:0] min_set;
  logic [ST_N_UNITS-1:0] age_set;

  logic [ST_UNIT_W-1:0]  sel;
  logic [ST_UNIT_W-1:0]  fixed_sel;
  logic [ST_OCC_W-1:0]   min_occ;
  logic [ST_AGE_W-1:0]   min_age;
  logic                  found;
  logic                  any_room;
  // The sizes of the three pools. They exist so that `grant_reason` names the
  // key that actually narrowed the pool and not merely a set that was equal to
  // another one.
  logic [31:0]           cnt_cand;
  logic [31:0]           cnt_use;
  logic [31:0]           cnt_min;

`ifdef MOSAIC_STEER_MUTANT_NONDET_AGE
  // MUTANT: a counter that is never reset. The tie-break then depends on how
  // many cycles the simulation has run, not on the grants the program caused.
  logic [ST_AGE_W-1:0] free_ctr_q;
  always_ff @(posedge clk) begin
    free_ctr_q <= free_ctr_q + {{(ST_AGE_W-1){1'b0}}, 1'b1};
  end
`endif

  // The class as an index. The mask is read with it, so an out-of-range class
  // cannot index past the matrix.
  logic [31:0] cls_idx;
  assign cls_idx = 32'(req_class);

  // ------------------------------------------------------- the three keys
  always_comb begin : keys
    cap_ok   = {ST_N_UNITS{1'b0}};
    cand     = {ST_N_UNITS{1'b0}};
    pref     = {ST_N_UNITS{1'b0}};
    use_set  = {ST_N_UNITS{1'b0}};
    min_set  = {ST_N_UNITS{1'b0}};
    age_set  = {ST_N_UNITS{1'b0}};
    min_occ  = {ST_OCC_W{1'b1}};
    min_age  = {ST_AGE_W{1'b1}};
    sel      = {ST_UNIT_W{1'b0}};
    found    = 1'b0;
    cnt_cand = 32'd0;
    cnt_use  = 32'd0;
    cnt_min  = 32'd0;

    for (int unsigned u = 0; u < ST_N_UNITS; u = u + 1) begin
`ifdef MOSAIC_STEER_MUTANT_IGNORE_CAPABILITY
      cap_ok[u] = 1'b1;
`else
      cap_ok[u] = unit_cap[u*ST_N_CLASSES + cls_idx];
`endif
`ifdef MOSAIC_STEER_MUTANT_SATURATION_ADMIT
      cand[u] = cap_ok[u];
`else
      cand[u] = cap_ok[u] && unit_room[u];
`endif
      pref[u] = cand[u] && ((req_locality_en == 1'b0) || unit_shared[u] ||
                            (unit_locality[u] == req_locality));
    end

    // Key 2: the preferred pool, or every candidate when there is no local one.
    if ((|pref) == 1'b1) begin
      use_set = pref;
    end else begin
      use_set = cand;
    end

    // Key 3: the least occupied member of the pool.
    for (int unsigned u = 0; u < ST_N_UNITS; u = u + 1) begin
      if (cand[u]) begin
        cnt_cand = cnt_cand + 32'd1;
      end
      if (use_set[u]) begin
        cnt_use = cnt_use + 32'd1;
      end
      if ((use_set[u] == 1'b1) && (unit_occ[u*ST_OCC_W +: ST_OCC_W] < min_occ)) begin
        min_occ = unit_occ[u*ST_OCC_W +: ST_OCC_W];
      end
    end
    for (int unsigned u = 0; u < ST_N_UNITS; u = u + 1) begin
      min_set[u] = use_set[u] && (unit_occ[u*ST_OCC_W +: ST_OCC_W] == min_occ);
      if (min_set[u]) begin
        cnt_min = cnt_min + 32'd1;
      end
    end

    // Key 4: the oldest last grant. The tie on the smallest index falls out of
    // `!found`, which is the only choice left that is both deterministic and
    // derived from nothing outside the offer.
    for (int unsigned u = 0; u < ST_N_UNITS; u = u + 1) begin
`ifdef MOSAIC_STEER_MUTANT_NONDET_AGE
      age_ref[u] = (free_ctr_q[1:0] == u[1:0]) ? ST_AGE_ZERO
                                              : {{(ST_AGE_W-1){1'b0}}, 1'b1};
`else
      age_ref[u] = last_age[u];
`endif
    end
    for (int unsigned u = 0; u < ST_N_UNITS; u = u + 1) begin
      if ((min_set[u] == 1'b1) && (age_ref[u] < min_age)) begin
        min_age = age_ref[u];
      end
    end
    for (int unsigned u = 0; u < ST_N_UNITS; u = u + 1) begin
      age_set[u] = min_set[u] && (age_ref[u] == min_age);
    end
    for (int unsigned u = 0; u < ST_N_UNITS; u = u + 1) begin
      if ((age_set[u] == 1'b1) && (found == 1'b0)) begin
        sel   = ST_UNIT_W'(u);
        found = 1'b1;
      end
    end
  end

  // ------------------------------------------------------------- the answer
  //
  // The offer is answered with exactly one of grant / stall / reject, in both
  // strategies:
  //
  //   reject  no unit in the machine implements the class at all -- a property
  //           of the class and the matrix, not of the strategy or the load;
  //   grant   a unit was chosen (dynamic: a capable one with room; fixed: the
  //           pinned one, if it can take the class and has room);
  //   stall   everything else: there is a unit that could take the class, and
  //           this cycle it is not available. The macro keeps its identity and
  //           is offered again next cycle. A stall never changes what the macro
  //           computes and never admits it anywhere.
  always_comb begin : answer
    fixed_sel    = fixed_unit[cls_idx*ST_UNIT_W +: ST_UNIT_W];
    any_room     = |cand;
    reject       = req_valid && ((|cap_ok) == 1'b0);

    if (mode_dyn == 1'b0) begin
      // The fixed baseline: the table's unit, if it can take the class and has
      // room. Nothing else is consulted, and a full pinned unit is a stall and
      // not a spill -- spilling is the strategy this mode exists to contrast
      // against.
      grant_valid  = req_valid && cap_ok[fixed_sel] && unit_room[fixed_sel];
      grant_unit   = fixed_sel;
      grant_reason = 3'd1;
    end else begin
      grant_valid = req_valid && ((|cap_ok) == 1'b1) && (any_room == 1'b1);
      grant_unit  = sel;
      // Which key was decisive: the last one that left a single unit.
      if (cnt_use == 32'd0) begin
        grant_reason = 3'd0;
      end else if (cnt_use == 32'd1 && cnt_cand > 32'd1) begin
        grant_reason = 3'd3;   // locality narrowed the pool to one
      end else if (cnt_use == 32'd1) begin
        grant_reason = 3'd2;   // the capability filter alone left one
      end else if (cnt_min == 32'd1) begin
        grant_reason = 3'd4;   // load picked the only least-occupied unit
      end else begin
        grant_reason = 3'd5;   // the age tie-break was needed
      end
    end

    stall = req_valid && !grant_valid && !reject;
  end

  // ------------------------------------------------------------- the state
  always_ff @(posedge clk) begin : ledger
    if (rst) begin
      for (int unsigned u = 0; u < ST_N_UNITS; u = u + 1) begin
        last_age[u]     <= ST_AGE_ZERO;
        o_unit_issues[u*32 +: 32] <= 32'd0;
      end
      o_grant_ctr  <= 32'd0;
      o_stall_ctr  <= 32'd0;
      o_reject_ctr <= 32'd0;
    end else begin
      if (grant_valid == 1'b1) begin
        last_age[grant_unit]      <= req_age;
        o_unit_issues[grant_unit*32 +: 32] <= o_unit_issues[grant_unit*32 +: 32] + ST_ONE;
      end
      if (req_valid && grant_valid) begin
        o_grant_ctr <= o_grant_ctr + ST_ONE;
      end
      if (stall) begin
        o_stall_ctr <= o_stall_ctr + ST_ONE;
      end
      if (reject) begin
        o_reject_ctr <= o_reject_ctr + ST_ONE;
      end
    end
  end

  // ------------------------------------------------------------- geometry
  assign o_units   = 32'(ST_N_UNITS);
  assign o_classes = 32'(ST_N_CLASSES);
  assign o_age_w   = 32'(ST_AGE_W);
  assign o_unit_w  = 32'(ST_UNIT_W);
  assign o_occ_w   = 32'(ST_OCC_W);

endmodule : mosaic_steering

`default_nettype wire
