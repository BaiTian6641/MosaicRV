// Simulation wrapper for CASE=steering.capacity_locality (work package I-029).
//
// The wrapper adds no timing and no policy of its own: `mosaic_steering` is a
// purely combinational decision with one register file (the per-unit last-grant
// age) and three counters, so the C++ driver drives the ports, evaluates with
// the clock low, compares the whole output surface against its own model of the
// contract, and then applies the edge. There is no clock generation, no reset
// generation and no `$display` here: the C++ side owns all three, per
// sim/common/sim_common.h.
//
// ------------------------------------------------------------ flattened pins
//
// Every driver-facing port is a fixed-width vector (32 bits), so the driver
// contains no geometry and would keep compiling if the policy's unit count, age
// width or occupancy width changed. Where the driver's vector is wider than the
// module's port (occupancy is 4 bits, a capability mask is 6 bits, a fixed-table
// entry is 2 bits), the narrowing is done here with the module's own parameters
// by scope reference, so the widths are named once, in the RTL.
//
// The module's own widths are read back as outputs (o_*_o below), so a change to
// them is a runtime failure in the driver rather than a silent truncation of a
// decision input.
//
// The capability matrix and the fixed table travel as one vector per unit and
// one vector per class respectively -- one pin each, packed in the same order
// the module indexes -- because a per-bit pin list would be a second, drifting
// copy of the matrix's shape.

/* verilator lint_off UNUSEDPARAM */
`include "mosaic_cfg_pkg.svh"
`include "mosaic_id_pkg.svh"
/* verilator lint_on UNUSEDPARAM */

`default_nettype none
`resetall

// The driver's pins are 32 bits wide by convention (see sim/common), so a few
// high bits of a pin carry nothing. That is the convention, not an oversight.
/* verilator lint_off UNUSEDSIGNAL */

module mosaic_steering_tb (
    input  logic        clk,
    input  logic        rst,

    // ------------------------------------------------------------- strategy
    input  logic        mode_dyn_i,

    // --------------------------------------------------- the offered macro
    input  logic        req_valid_i,
    input  logic [31:0] req_class_i,
    input  logic        req_locality_i,
    input  logic        req_locality_en_i,
    input  logic [31:0] req_age_i,

    // ---------------------------------------------------- the machine state
    input  logic        unit_room0_i,
    input  logic        unit_room1_i,
    input  logic        unit_room2_i,
    input  logic        unit_room3_i,
    input  logic [31:0] unit_occ0_i,
    input  logic [31:0] unit_occ1_i,
    input  logic [31:0] unit_occ2_i,
    input  logic [31:0] unit_occ3_i,
    input  logic [31:0] unit_cap0_i,
    input  logic [31:0] unit_cap1_i,
    input  logic [31:0] unit_cap2_i,
    input  logic [31:0] unit_cap3_i,
    input  logic [31:0] unit_locality_i,
    input  logic [31:0] unit_shared_i,
    input  logic [63:0] fixed_unit_i,

    // ------------------------------------------------------------- decision
    output logic        grant_valid_o,
    output logic [31:0] grant_unit_o,
    output logic [31:0] grant_reason_o,
    output logic        stall_o,
    output logic        reject_o,

    // ---------------------------------------------------------- observation
    output logic [31:0] o_unit_issues0_o,
    output logic [31:0] o_unit_issues1_o,
    output logic [31:0] o_unit_issues2_o,
    output logic [31:0] o_unit_issues3_o,
    output logic [31:0] o_grant_ctr_o,
    output logic [31:0] o_stall_ctr_o,
    output logic [31:0] o_reject_ctr_o,

    // ------------------------------------------------------------- geometry
    output logic [31:0] o_units_o,
    output logic [31:0] o_classes_o,
    output logic [31:0] o_age_w_o,
    output logic [31:0] o_unit_w_o,
    output logic [31:0] o_occ_w_o
);

  // The widths this wrapper must fix are the module's defaults. They are
  // written here as literals because a wrapper has to size its own pins, and
  // the module's geometry is read back through `o_*_o` and checked by the
  // driver against the same numbers, so a drift between the two is a runtime
  // failure rather than a silent truncation.
  localparam int unsigned N_UNITS   = 4;   // mosaic_steering.ST_N_UNITS
  localparam int unsigned N_CLASSES = 6;   // mosaic_steering.ST_N_CLASSES
  localparam int unsigned AGE_W     = 16;  // mosaic_steering.ST_AGE_W
  localparam int unsigned OCC_W     = 4;   // mosaic_steering.ST_OCC_W
  localparam int unsigned UNIT_W    = 2;   // mosaic_steering.ST_UNIT_W

  logic [N_UNITS*OCC_W-1:0]      unit_occ_packed;
  logic [N_UNITS*N_CLASSES-1:0]  unit_cap_packed;
  logic [N_UNITS-1:0]            unit_room_packed;
  logic [N_CLASSES*UNIT_W-1:0]   fixed_unit_packed;
  logic [N_UNITS*32-1:0]         unit_issues_flat;
  logic [UNIT_W-1:0]             grant_unit_w;
  logic [2:0]                    grant_reason_w;

  assign unit_room_packed = {unit_room3_i, unit_room2_i, unit_room1_i, unit_room0_i};

  assign unit_occ_packed = {OCC_W'(unit_occ3_i), OCC_W'(unit_occ2_i),
                            OCC_W'(unit_occ1_i), OCC_W'(unit_occ0_i)};

  assign unit_cap_packed = {unit_cap3_i[N_CLASSES-1:0], unit_cap2_i[N_CLASSES-1:0],
                            unit_cap1_i[N_CLASSES-1:0], unit_cap0_i[N_CLASSES-1:0]};

  assign fixed_unit_packed = {fixed_unit_i[5*8 +: UNIT_W], fixed_unit_i[4*8 +: UNIT_W],
                              fixed_unit_i[3*8 +: UNIT_W], fixed_unit_i[2*8 +: UNIT_W],
                              fixed_unit_i[1*8 +: UNIT_W], fixed_unit_i[0*8 +: UNIT_W]};

  mosaic_steering u_steering (
      .clk           (clk),
      .rst           (rst),
      .mode_dyn      (mode_dyn_i),

      .req_valid     (req_valid_i),
      .req_class     (mosaic_uop_pkg::uop_class_e'(req_class_i[2:0])),
      .req_locality  (req_locality_i),
      .req_locality_en(req_locality_en_i),
      .req_age       (AGE_W'(req_age_i)),

      .unit_room     (unit_room_packed),
      .unit_occ      (unit_occ_packed),
      .unit_cap      (unit_cap_packed),
      .unit_locality (N_UNITS'(unit_locality_i)),
      .unit_shared   (N_UNITS'(unit_shared_i)),
      .fixed_unit    (fixed_unit_packed),

      .grant_valid   (grant_valid_o),
      .grant_unit    (grant_unit_w),
      .grant_reason  (grant_reason_w),
      .stall         (stall_o),
      .reject        (reject_o),

      .o_unit_issues (unit_issues_flat),
      .o_grant_ctr   (o_grant_ctr_o),
      .o_stall_ctr   (o_stall_ctr_o),
      .o_reject_ctr  (o_reject_ctr_o),

      .o_units       (o_units_o),
      .o_classes     (o_classes_o),
      .o_age_w       (o_age_w_o),
      .o_unit_w      (o_unit_w_o),
      .o_occ_w       (o_occ_w_o)
  );

  assign grant_unit_o   = {30'd0, grant_unit_w};
  assign grant_reason_o = {29'd0, grant_reason_w};

  assign o_unit_issues0_o = unit_issues_flat[0*32 +: 32];
  assign o_unit_issues1_o = unit_issues_flat[1*32 +: 32];
  assign o_unit_issues2_o = unit_issues_flat[2*32 +: 32];
  assign o_unit_issues3_o = unit_issues_flat[3*32 +: 32];

/* verilator lint_on UNUSEDSIGNAL */

endmodule : mosaic_steering_tb

`default_nettype wire
