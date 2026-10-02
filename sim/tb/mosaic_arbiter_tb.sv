// ============================================================================
// Simulation wrapper for CASE=arbiter.forward_progress (work package I-030).
//
// The wrapper adds no timing and no policy of its own: `mosaic_arbiter` is
// combinational on its outputs from registers, so the C++ driver drives the
// ports, evaluates with the clock low, compares the outputs against its own
// model of the environment, and then applies the edge. There is no clock
// generation, no reset generation and no `$display` in here; the C++ side owns
// all three, per sim/common/sim_common.h.
//
// ------------------------------------------------------------ flattened pins
//
// Every driver-facing port is a fixed-width vector (32 bits per scalar, four
// 32-bit lanes for the vector ports), so the driver contains no geometry and
// would keep compiling if the policy's class count, queue depth or counter
// widths changed. The module's own widths are fixed as literals here -- a
// wrapper has to size its pins -- and the module reads its geometry *back* on
// the `o_*` ports, so a drift between the two is a runtime failure in the
// driver rather than a silent truncation.
//
// The module declares no package dependency, so this file needs none either:
// the request tags are 8-bit bytes and every other port is a plain logic
// vector.
// ============================================================================

`default_nettype none
`resetall

// The driver's pins are 32 bits wide by convention (see sim/common), so the high
// bits of a few of them carry nothing. That is the convention, not an oversight,
// and it is silenced around the port list rather than left to be inferred.
/* verilator lint_off UNUSEDSIGNAL */

module mosaic_arbiter_tb (
    input  logic        clk,
    input  logic        rst,

    // ------------------------------------------------------ request classes
    input  logic [3:0]  req_valid_i,
    input  logic [31:0] req_src0_i,
    input  logic [31:0] req_src1_i,
    input  logic [31:0] req_src2_i,
    input  logic [31:0] req_src3_i,
    output logic [3:0]  req_ready_o,

    // --------------------------------------------------------- service port
    input  logic        srv_busy_i,
    output logic        srv_go_o,
    output logic [31:0] srv_class_o,
    output logic [31:0] srv_src_o,

    // -------------------------------------------------- window observation
    output logic [31:0] o_window_index_o,
    output logic [31:0] o_window_grants_o,
    output logic [31:0] o_used0_o,
    output logic [31:0] o_used1_o,
    output logic [31:0] o_used2_o,
    output logic [31:0] o_used3_o,
    output logic [31:0] o_pend0_o,
    output logic [31:0] o_pend1_o,
    output logic [31:0] o_pend2_o,
    output logic [31:0] o_pend3_o,

    // -------------------------------------------------------------- counters
    output logic [31:0] o_grant_ctr_o,
    output logic [31:0] o_escape_ctr_o,
    output logic [31:0] o_accept_ctr_o,
    output logic [31:0] o_refuse_ctr_o,
    output logic [31:0] o_stall_ctr_o,
    output logic [31:0] o_drop_ctr_o,
    output logic [31:0] o_busy_run_max_o,
    output logic        o_assumption_violated_o,

    // -------------------------------------------------- geometry read-back
    output logic [31:0] o_classes_o,
    output logic [31:0] o_window_o,
    output logic [31:0] o_d_max_o,
    output logic [31:0] o_depth_o,
    output logic [31:0] o_quota0_o,
    output logic [31:0] o_quota1_o,
    output logic [31:0] o_quota2_o,
    output logic [31:0] o_quota3_o
);

  logic [3:0][7:0]  req_src_flat;
  logic [1:0]       srv_class_w;
  logic [7:0]       srv_src_w;
  logic [3:0][31:0] used_flat;
  logic [3:0][31:0] pend_flat;
  logic [3:0][31:0] quota_flat;

  assign req_src_flat = {8'(req_src3_i), 8'(req_src2_i),
                         8'(req_src1_i), 8'(req_src0_i)};

  mosaic_arbiter u_arbiter (
      .clk        (clk),
      .rst        (rst),

      .req_valid  (req_valid_i),
      .req_ready  (req_ready_o),
      .req_src    (req_src_flat),

      .srv_go     (srv_go_o),
      .srv_class  (srv_class_w),
      .srv_src    (srv_src_w),
      .srv_busy   (srv_busy_i),

      .o_window_index  (o_window_index_o),
      .o_window_grants (o_window_grants_o),
      .o_used          (used_flat),
      .o_pend_count    (pend_flat),

      .o_grant_ctr    (o_grant_ctr_o),
      .o_escape_ctr   (o_escape_ctr_o),
      .o_accept_ctr   (o_accept_ctr_o),
      .o_refuse_ctr   (o_refuse_ctr_o),
      .o_stall_ctr    (o_stall_ctr_o),
      .o_drop_ctr     (o_drop_ctr_o),
      .o_busy_run_max (o_busy_run_max_o),
      .o_assumption_violated (o_assumption_violated_o),

      .o_classes  (o_classes_o),
      .o_window   (o_window_o),
      .o_d_max    (o_d_max_o),
      .o_depth    (o_depth_o),
      .o_quota    (quota_flat)
  );

  assign srv_class_o = {30'd0, srv_class_w};
  assign srv_src_o   = {24'd0, srv_src_w};

  assign o_used0_o = used_flat[0];
  assign o_used1_o = used_flat[1];
  assign o_used2_o = used_flat[2];
  assign o_used3_o = used_flat[3];

  assign o_pend0_o = pend_flat[0];
  assign o_pend1_o = pend_flat[1];
  assign o_pend2_o = pend_flat[2];
  assign o_pend3_o = pend_flat[3];

  assign o_quota0_o = quota_flat[0];
  assign o_quota1_o = quota_flat[1];
  assign o_quota2_o = quota_flat[2];
  assign o_quota3_o = quota_flat[3];

  /* verilator lint_on UNUSEDSIGNAL */

endmodule : mosaic_arbiter_tb

/* verilator lint_on UNUSEDSIGNAL */

`default_nettype wire
