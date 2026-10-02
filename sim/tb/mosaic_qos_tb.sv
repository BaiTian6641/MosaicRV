// ============================================================================
// Simulation wrapper for CASE=memory.qos_no_starvation (work package I-062).
//
// The wrapper adds no timing and no policy of its own: `mosaic_mem_qos` is
// combinational on its outputs from registers, so the C++ driver drives the
// ports, evaluates with the clock low, compares the outputs against its own
// model of the environment and of the policy rule, and then applies the edge.
// There is no clock generation, no reset generation and no `$display` in here;
// the C++ side owns all three, per sim/common/sim_common.h.
//
// ------------------------------------------------------------ flattened pins
//
// Every driver-facing port is a fixed-width vector (32 bits per scalar, three
// 32-bit lanes for the per-class vectors), so the driver contains no geometry
// and would keep compiling if the policy's class count, queue depth or counter
// widths changed. The module's own widths are fixed as literals here -- a
// wrapper has to size its pins -- and the module reads its geometry *back* on
// the `o_*` ports, so a drift between the two is a runtime failure in the
// driver rather than a silent truncation.
// ============================================================================

`default_nettype none
`resetall

// The driver's pins are 32 bits wide by convention (see sim/common), so the high
// bits of a few of them carry nothing. That is the convention, not an oversight,
// and it is silenced around the port list rather than left to be inferred.
/* verilator lint_off UNUSEDSIGNAL */

module mosaic_qos_tb (
    input  logic        clk,
    input  logic        rst,

    // -------------------------------------------------------- policy select
    input  logic        qos_en_i,

    // ------------------------------------------------------ request classes
    input  logic [2:0]  req_valid_i,
    input  logic [31:0] req_tag0_i,
    input  logic [31:0] req_tag1_i,
    input  logic [31:0] req_tag2_i,
    input  logic [31:0] req_data0_i,
    input  logic [31:0] req_data1_i,
    input  logic [31:0] req_data2_i,
    output logic [2:0]  req_ready_o,

    // --------------------------------------------------------- service port
    input  logic        srv_busy_i,
    output logic        srv_go_o,
    output logic [31:0] srv_class_o,
    output logic [31:0] srv_tag_o,
    output logic [31:0] srv_data_o,

    // -------------------------------------------------- window observation
    output logic [31:0] o_window_index_o,
    output logic [31:0] o_window_grants_o,
    output logic [31:0] o_used0_o,
    output logic [31:0] o_used1_o,
    output logic [31:0] o_used2_o,
    output logic [31:0] o_pend0_o,
    output logic [31:0] o_pend1_o,
    output logic [31:0] o_pend2_o,

    // -------------------------------------------------------------- counters
    output logic [31:0] o_grant_ctr_o,
    output logic [31:0] o_escape_ctr_o,
    output logic [31:0] o_accept_ctr_o,
    output logic [31:0] o_refuse_ctr_o,
    output logic [31:0] o_stall_ctr_o,
    output logic [31:0] o_idle_ctr_o,
    output logic [31:0] o_drop_ctr_o,
    output logic [31:0] o_busy_run_max_o,
    output logic        o_assumption_violated_o,

    // -------------------------------------------------- geometry read-back
    output logic [31:0] o_classes_o,
    output logic [31:0] o_window_o,
    output logic [31:0] o_d_max_o,
    output logic [31:0] o_depth_o,
    output logic [31:0] o_age_limit_o,
    output logic [31:0] o_quota0_o,
    output logic [31:0] o_quota1_o,
    output logic [31:0] o_quota2_o
);

  logic [2:0][7:0]  req_tag_flat;
  logic [2:0][31:0] req_data_flat;
  logic [1:0]       srv_class_w;
  logic [7:0]       srv_tag_w;
  logic [2:0][31:0] used_flat;
  logic [2:0][31:0] pend_flat;
  logic [2:0][31:0] quota_flat;

  assign req_tag_flat  = {8'(req_tag2_i), 8'(req_tag1_i), 8'(req_tag0_i)};
  assign req_data_flat = {req_data2_i, req_data1_i, req_data0_i};

  mosaic_mem_qos u_qos (
      .clk        (clk),
      .rst        (rst),

      .qos_en     (qos_en_i),

      .req_valid  (req_valid_i),
      .req_ready  (req_ready_o),
      .req_tag    (req_tag_flat),
      .req_data   (req_data_flat),

      .srv_go     (srv_go_o),
      .srv_class  (srv_class_w),
      .srv_tag    (srv_tag_w),
      .srv_data   (srv_data_o),
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
      .o_idle_ctr     (o_idle_ctr_o),
      .o_drop_ctr     (o_drop_ctr_o),
      .o_busy_run_max (o_busy_run_max_o),
      .o_assumption_violated (o_assumption_violated_o),

      .o_classes   (o_classes_o),
      .o_window    (o_window_o),
      .o_d_max     (o_d_max_o),
      .o_depth     (o_depth_o),
      .o_age_limit (o_age_limit_o),
      .o_quota     (quota_flat)
  );

  assign srv_class_o = {30'd0, srv_class_w};
  assign srv_tag_o   = {24'd0, srv_tag_w};

  assign o_used0_o = used_flat[0];
  assign o_used1_o = used_flat[1];
  assign o_used2_o = used_flat[2];

  assign o_pend0_o = pend_flat[0];
  assign o_pend1_o = pend_flat[1];
  assign o_pend2_o = pend_flat[2];

  assign o_quota0_o = quota_flat[0];
  assign o_quota1_o = quota_flat[1];
  assign o_quota2_o = quota_flat[2];

  /* verilator lint_on UNUSEDSIGNAL */

endmodule : mosaic_qos_tb

/* verilator lint_on UNUSEDSIGNAL */

`default_nettype wire
